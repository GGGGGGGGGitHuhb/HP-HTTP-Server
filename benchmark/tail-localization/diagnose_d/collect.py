"""Fixed D diagnostics. Preparation grants no live collection permission."""
import argparse
import errno
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import signal
import stat
import subprocess
import sys
import threading
import time
sys.dont_write_bytecode=True
ROOT=Path(__file__).absolute().parents[3]
STAGE=ROOT/'.cache/v0.5.1-revalidation'
BASE=ROOT/'.cache/v0.5.1-s4/builder/run-r015-build-001/build-output'
CLIENT=BASE/'relocated/client/wrk-baseline'
PACKAGE=BASE/'relocated-package'
CLIENT_SHA='1f0065792d9370fd14689caa7a9df5247f44bbb4c2066d780efbd94001adf348'
SERVER_SHA='46cb6a39410b819f1b1f156c3db9d2eb9f4da8ef73c8f61f979c8ae05125b240'
PAYLOAD_SHA='785b0751fc2c53dc14a4ce3d800e69ef9ce1009eb327ccf458afe09c242c26c9'


def own_module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


adapter=own_module('r033_scope',ROOT/'benchmark/revalidation/revalidate.py')
analysis=own_module('r033_analyze',Path(__file__).with_name('analyze.py'))
require=adapter.require


def publish(path,value):
    with Path(path).open('x') as stream:
        json.dump(value,stream,indent=2)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())


def first_failure(errors,error):
    try:
        message=str(error)
    except BaseException:
        message='unavailable'
    errors.append({'type':type(error).__name__,'message':message})
    try:
        print(errors[-1],file=sys.stderr)
    except BaseException:
        pass


def stat_identity(pid,tid=None):
    path=Path('/proc')/str(pid)
    if tid is not None:
        path=path/'task'/str(tid)
    try:
        raw=(path/'stat').read_text()
    except OSError as error:
        if error.errno in (errno.ENOENT,errno.ESRCH):
            return None
        raise
    fields=raw[raw.rfind(')')+2:].split()
    require(len(fields)>21,'stat parse')
    return {'pid':pid,'tid':tid or pid,'starttime':int(fields[19]),'run_ticks':int(fields[11])+int(fields[12])}


def thread_snapshot(targets):
    sample={'begin_ns':time.monotonic_ns(),'threads':[],'errors':[]}
    for role,identity in targets:
        pid=identity['pid']
        before=stat_identity(pid)
        require(before is not None and before['starttime']==identity['starttime'],'target exited/reused')
        for entry in sorted((Path('/proc')/str(pid)/'task').iterdir(),key=lambda p:int(p.name)):
            tid=int(entry.name)
            first=stat_identity(pid,tid)
            require(first is not None,'thread disappeared during read')
            sched=list(map(int,(entry/'schedstat').read_text().split()))
            status={line.split(':')[0]:line.split(':',1)[1].strip() for line in (entry/'status').read_text().splitlines() if ':' in line}
            io={line.split(':')[0]:int(line.split(':')[1]) for line in (entry/'io').read_text().splitlines()}
            wchan=(entry/'wchan').read_text().strip()
            after=stat_identity(pid,tid)
            require(after is not None and after['starttime']==first['starttime'],'thread identity unknown')
            sample['threads'].append({'role':role,'pid':pid,'process_starttime':identity['starttime'],'tid':tid,
                'thread_starttime':first['starttime'],'run_ticks':after['run_ticks'],'sched_run_ns':sched[0],
                'sched_wait_ns':sched[1],'voluntary':int(status['voluntary_ctxt_switches']),
                'nonvoluntary':int(status['nonvoluntary_ctxt_switches']),'read_bytes':io['read_bytes'],
                'write_bytes':io['write_bytes'],'wchan':wchan})
        after=stat_identity(pid)
        require(after is not None and after['starttime']==identity['starttime'],'process identity unknown')
    sample['end_ns']=time.monotonic_ns()
    return sample


def network_snapshot(port):
    sample={'begin_ns':time.monotonic_ns(),'errors':[],'system':{},'tcp_tuples':[]}
    for name in ('stat','loadavg','pressure/cpu','pressure/io','net/snmp','net/netstat'):
        sample['system'][name]=(Path('/proc')/name).read_text()
    for line in (Path('/proc/net/tcp')).read_text().splitlines()[1:]:
        fields=line.split()
        if int(fields[1].split(':')[1],16)==port or int(fields[2].split(':')[1],16)==port:
            sample['tcp_tuples'].append({'local':fields[1],'remote':fields[2],'state':fields[3],'queues':fields[4]})
    sample['end_ns']=time.monotonic_ns()
    return sample


def sample_observer(targets,port,output,stop,errors,deadline):
    next_thread=next_network=time.monotonic_ns()
    streams=[]
    try:
        threads=(output/'threads.jsonl').open('x');streams.append(threads)
        network=(output/'network.jsonl').open('x');streams.append(network)
        try:
            while not stop.is_set():
                now=time.monotonic_ns()
                require(now<deadline,'observer deadline')
                if now>=next_thread:
                    threads.write(json.dumps(thread_snapshot(targets))+'\n')
                    threads.flush()
                    next_thread=now+10000000
                if now>=next_network:
                    network.write(json.dumps(network_snapshot(port))+'\n')
                    network.flush()
                    next_network=now+100000000
                require(sum(p.stat().st_size for p in output.iterdir() if p.is_file() and p.suffix not in ('.stdout','.stderr','.log')) < 192*1024*1024,'raw capacity')
                stop.wait(.001)
        except BaseException as error:
            first_failure(errors,error)
            stop.set()
    except BaseException as error:
        first_failure(errors,error)
        stop.set()
    finally:
        for stream in streams:
            try:stream.close()
            except BaseException as error:first_failure(errors,error)
        if not stop.is_set():
            first_failure(errors,RuntimeError('observer exited without stop request'))
            stop.set()


def fixed_commands(role,output,port):
    server=STAGE/role/'D/build/hp_http_server'
    env={'PATH':'/usr/bin:/bin','LD_LIBRARY_PATH':str(PACKAGE/'runtime'),
         'LUA_PATH':str(PACKAGE/'runtime/lua/?.lua')+';'+str(PACKAGE/'runtime/lua/?/init.lua'),
         'LUA_CPATH':'','HP_BASELINE_WARMUP_SECONDS':'5','HP_BASELINE_OUTPUT_DIR':str(output)}
    for key in ('TMPDIR','TMP','TEMP','XDG_CACHE_HOME','PYTHONDONTWRITEBYTECODE'):
        env[key]=os.environ[key]
    return {'server':[str(server),'--threads','4','--idle-timeout-ms','30000','--keep-alive-timeout-ms','15000',
            '--shutdown-timeout-ms','5000','--port','0','--root',str(output/'document-root')],
            'client':[str(CLIENT),'-t2','-c128','-d','20s','--timeout','2s',
                      'http://127.0.0.1:%d/payload-1024.bin'%port], 'client_environment':env}


def audit_five(legacy,port,payload):
    return legacy.audit(port,payload,count=5)


def check_capacity(logical,allocated,limit):
    require(type(logical) is int and type(allocated) is int and min(logical,allocated)>=0
            and max(logical,allocated)<=limit,'new material capacity')


def finish_contract(output,scope,result,errors):
    cleanup=None
    try:cleanup=scope.finish()
    except BaseException as error:first_failure(errors,error)
    try:
        publish(output/'contract-result.json',{'schema':'r033-contract-v1','status':'valid' if not errors and cleanup and cleanup['complete'] else 'invalid',
            'fixture':result,'first_error':errors[0] if errors else None,'errors':errors,'cleanup':cleanup,
            'formal_sampling_authorized':False,'actual_D_started':False})
    except BaseException as error:first_failure(errors,error)
    return cleanup


def collect_live(role,mode,output,scope):
    # Reachable only through a future separately approved authority, never approval1.
    server=STAGE/role/'D/build/hp_http_server'
    require(adapter.digest(server)==SERVER_SHA and adapter.digest(CLIENT)==CLIENT_SHA,'fixed binaries')
    manifest=json.loads((STAGE/role/'D/manifest.json').read_text())
    adapter.validate_manifest(manifest,role,'D',scope)
    payload=analysis.reference.read_regular(PACKAGE/'config/payload-1024.bin',1024)
    require(hashlib.sha256(payload).hexdigest()==PAYLOAD_SHA,'payload identity')
    (output/'document-root').mkdir()
    (output/'document-root/payload-1024.bin').write_bytes(payload)
    legacy=adapter.load_legacy(scope)
    streams=[]
    stop=threading.Event()
    observer=None
    errors=[]
    original=None
    result=None
    try:
        for name in ('server.stdout','server.stderr','client.stdout','client.stderr'):
            streams.append((output/name).open('x'))
        commands=fixed_commands(role,output,0)
        process=subprocess.Popen(commands['server'],stdout=streams[0],stderr=streams[1],env=os.environ.copy())
        server_item=scope.register(process)
        until=min(scope.work_deadline,time.monotonic()+3)
        port=None
        while time.monotonic()<until:
            scope.poll()
            require(process.poll() is None,'server early exit')
            text=(output/'server.stdout').read_text()+(output/'server.stderr').read_text()
            import re
            matches=re.findall(r'listening on port (\d+)\.',text)
            if matches:
                require(len(matches)==1,'unique listen line')
                port=int(matches[0]);break
            time.sleep(.01)
        require(port is not None,'startup timeout')
        require(legacy.listener_owned(server_item['identity']['pid'],port),'listener PID ownership')
        audit_payload={'name':'payload-1024.bin','size':1024,'sha256':PAYLOAD_SHA}
        pre=audit_five(legacy,port,audit_payload)
        commands=fixed_commands(role,output,port)
        publish(output/'actual-command.json',commands)
        process_client=subprocess.Popen(commands['client'],stdout=streams[2],stderr=streams[3],env=commands['client_environment'])
        client_item=scope.register(process_client)
        if mode=='observe':
            targets=[('server',server_item['identity']),('client',client_item['identity']),('observer',stat_identity(os.getpid()))]
            observer=threading.Thread(target=sample_observer,args=(targets,port,output,stop,errors,int(scope.work_deadline*1e9)))
            observer.start()
        while process_client.poll() is None:
            scope.poll()
            require(not errors,'observer read failure')
            time.sleep(.005)
        require(process_client.wait()==0,'client invalid')
        stop.set()
        if observer is not None:
            observer.join(timeout=max(0,scope.cleanup_deadline-time.monotonic()))
            require(not observer.is_alive() and not errors,'observer cleanup unknown')
        post=audit_five(legacy,port,audit_payload)
        client=json.loads(analysis.reference.read_regular(output/'client.json',4*1024*1024))
        validated=analysis.validate_client(client,[analysis.reference.decode_bins(analysis.reference.read_regular(output/name,4*1024*1024)) for name in analysis.reference.NAMES])
        if mode=='observe':
            load=lambda name:[json.loads(line) for line in analysis.reference.read_regular(output/name,192*1024*1024).splitlines()]
            correlation=analysis.correlate(validated['slow_records'],load('threads.jsonl'),load('network.jsonl'),client['T0_ns'],client['T1_ns'])
            require(correlation['direction_status']=='descriptive_evidence','observation coverage unknown')
        require(client['actual_LD_LIBRARY_PATH']==commands['client_environment']['LD_LIBRARY_PATH'] and
                client['actual_LUA_PATH']==commands['client_environment']['LUA_PATH'] and client['pid']==process_client.pid,'actual client env/PID')
        result={'validated':validated,'pre_audit':pre,'post_audit':post,'commands':commands,'mode':mode}
        return result
    except BaseException as error:
        original=error
        raise
    finally:
        stop.set()
        if observer is not None:
            try:
                observer.join(timeout=max(0,scope.cleanup_deadline-time.monotonic()))
                require(not observer.is_alive(),'observer still alive')
            except BaseException as error:
                first_failure(errors,error)
        cleanup=None
        try:
            cleanup=scope.finish()
            require(cleanup['complete'] and not cleanup['remaining'],'live cleanup incomplete')
        except BaseException as error:first_failure(errors,error)
        for stream in streams:
            try:stream.close()
            except BaseException as error:first_failure(errors,error)
        try:
            catalog=[]
            for path in sorted(output.iterdir()):
                if path.is_file():
                    metadata=path.lstat()
                    require(stat.S_ISREG(metadata.st_mode),'raw regular artifact')
                    catalog.append({'path':path.name,'sha256':adapter.digest(path),'bytes':metadata.st_size})
            if mode=='observe':
                require({'threads.jsonl','network.jsonl'} <= {row['path'] for row in catalog},'observation catalog missing')
            publish(output/'sample.json',{'schema':'r033-d-sample-v1','status':'valid' if original is None and not errors else 'invalid',
                'role':role,'mode':mode,'server_commit':'69424e6ab057bba2950c018e34c5695a4dc74f22',
                'server_sha256':SERVER_SHA,'client_sha256':CLIENT_SHA,'payload_sha256':PAYLOAD_SHA,
                'result':result,'cleanup':cleanup,'errors':errors,
                'first_error':{'type':type(original).__name__} if original is not None else (errors[0] if errors else None),
                'catalog':catalog,'clock':'CLOCK_MONOTONIC','cause_confirmed':False})
        except BaseException as error:first_failure(errors,error)
        if errors and original is None:
            raise ValueError('observer/resource errors: '+repr(errors))


def parse_arguments(argv=None):
    parser=argparse.ArgumentParser()
    parser.add_argument('--role',choices=('builder','reviewer'),required=True)
    parser.add_argument('--output',required=True)
    parser.add_argument('--mode',choices=('entry','base','observe'),required=True)
    parser.add_argument('--shared-work-deadline-ns',type=int,required=True)
    parser.add_argument('--shared-cleanup-deadline-ns',type=int,required=True)
    return parser.parse_args(argv)


def main():
    args=parse_arguments()
    output=Path(args.output).absolute()
    require(output==STAGE/args.role/('control/r034-diagnose-'+args.mode+'-driver-001'),'exact mode output')
    require(args.role=='builder' or args.mode=='entry','Reviewer cannot collect')
    adapter.SHARED_WORK_DEADLINE=args.shared_work_deadline_ns/1e9
    adapter.SHARED_CLEANUP_DEADLINE=args.shared_cleanup_deadline_ns/1e9-2
    require(time.monotonic()<adapter.SHARED_WORK_DEADLINE<adapter.SHARED_CLEANUP_DEADLINE,'common deadline')
    output.mkdir()
    outer=own_module('r034_measure',ROOT/'benchmark/revalidation/execute_check.py')
    formal_logs={STAGE/args.role/('control/r034-diagnose-'+mode+'-driver-001')/name
                 for mode in ('base','observe') for name in ('server.stdout','server.stderr','client.stdout','client.stderr')}
    def measure(root):
        accounting={}
        outer.bytes_under(root,accounting,formal_logs)
        return {'allocated_bytes':accounting['allocated'],'logical_bytes':accounting['logical'],'log_logical_bytes':accounting['logs']+accounting['formal_logs']}
    adapter.measure=measure  # Only this private loaded Scope instance; frozen source stays unchanged.
    scope=adapter.Scope(STAGE/args.role,10 if args.mode=='entry' else 35,14 if args.mode=='entry' else 39)
    errors=[]
    result=None
    cleanup=None
    old_handler=signal.getsignal(signal.SIGTERM)
    def interrupted(signum,frame):raise TimeoutError('preparation TERM')
    try:
        signal.signal(signal.SIGTERM,interrupted)
        scope.poll()
        if args.mode=='entry':
            tests=own_module('r034_fixture',Path(__file__).with_name('test_diagnose.py'))
            result=tests.run_entry(output,scope)
            require(result['failures']==0 and result['errors']==0,'entry fixture failure')
        else:
            collect_live(args.role,args.mode,output,scope)
    except BaseException as error:
        first_failure(errors,error)
    finally:
        try:signal.signal(signal.SIGTERM,signal.SIG_IGN)
        except BaseException as error:first_failure(errors,error)
        if args.mode=='entry':
            cleanup=finish_contract(output,scope,result,errors)
        else:
            try:cleanup=scope.finish()
            except BaseException as error:first_failure(errors,error)
        try:signal.signal(signal.SIGTERM,old_handler)
        except BaseException as error:first_failure(errors,error)
    require(not errors and cleanup and cleanup['complete'],'driver invalid/unknown')


if __name__=='__main__':main()
