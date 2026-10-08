"""Power-identity diagnostic sample. Only executed inside an admitted root marker launcher."""
import argparse
import ctypes
import hashlib
import json
import mmap
import os
from pathlib import Path
import re
import resource
import signal
import shutil
import socket
import subprocess
import time
from wire_types import Atomics, Control, Connection, Thread, identity, plain, write_json

BASE = Path(__file__).resolve().parents[2]
ROLE = BASE / '.cache/v0.5.1-s4/builder'


def resource_gate(output,phase):
    began=time.monotonic_ns()
    available=None
    for line in Path('/proc/meminfo').read_text().splitlines():
        if line.startswith('MemAvailable:'):
            fields=line.split()
            demand(len(fields)==3 and fields[2]=='kB','MemAvailable units unknown')
            available=int(fields[1])*1024
    disk=shutil.disk_usage(output)
    nofile=resource.getrlimit(resource.RLIMIT_NOFILE)
    evidence=dict(read_start_ns=began,read_end_ns=time.monotonic_ns(),mem_available_bytes=available,
                  disk_free_bytes=disk.free,nofile=list(nofile),output_filesystem=str(output))
    write_json(output/f'resources-{phase}.json',evidence)
    demand(available is not None and available>=1024**3,'MemAvailable below 1GiB/unknown')
    demand(disk.free>=4*1024**3,'output disk free below 4GiB')
    demand(nofile[0]==resource.RLIM_INFINITY or nofile[0]>=1024,'power nofile below 1024')
    return evidence


def demand(value, message):
    if not value:
        raise RuntimeError(message)


def alive(record):
    try:
        current=identity(record['pid'])
        return current['starttime'] == record['starttime'] and current['state']!='Z'
    except FileNotFoundError:
        return False


def tuple_key(record, server=False):
    if server:
        return (record.remoteAddress,record.remotePort,record.localAddress,record.localPort)
    return (record.localAddress,record.localPort,record.remoteAddress,record.remotePort)


def freeze(control, atomics):
    count=control.connections
    servers=[control.serverConnections[index] for index in range(count)]
    clients=[control.clientConnections[index] for index in range(count)]
    demand(all(atomics.load(item,'ready') == 1 for item in servers+clients),'connection metadata incomplete')
    by_tuple={tuple_key(item,True):item for item in servers}
    demand(len(by_tuple)==count and len({tuple_key(item) for item in clients})==count,'duplicate connection tuple')
    pairs=[];snapshots={}
    for item in clients:
        server=by_tuple.get(tuple_key(item))
        demand(server is not None,'missing opposite endpoint')
        demand(not item.closed and not server.closed,'connection closed before freeze')
        for record in (item,server):
            before_ns=time.monotonic_ns()
            actual=identity(record.pid,record.tid)
            actual['snapshot_start_ns']=before_ns;actual['snapshot_end_ns']=time.monotonic_ns()
            demand(actual['starttime']==record.starttime,'thread identity drift')
            snapshots[(record.pid,record.tid)]=actual
        pairs.append((server,item))
    # Identity-only rule: one per server worker; replace within that worker to cover both client workers.
    chosen=[]
    for worker in range(4):
        choices=sorted((pair for pair in pairs if pair[0].worker==worker),key=lambda pair:pair[1].index)
        demand(choices,'server worker not represented in frozen connections')
        chosen.append(choices[0])
    for worker in range(2):
        if not any(pair[1].worker==worker for pair in chosen):
            choices=sorted((pair for pair in pairs if pair[1].worker==worker),key=lambda pair:pair[1].index)
            demand(choices,'client worker not represented')
            replacement=choices[0]
            position=next(index for index,pair in enumerate(chosen) if pair[0].worker==replacement[0].worker)
            chosen[position]=replacement
    demand(len(chosen)<=16,'selection cap')
    control.selectedCount=len(chosen)
    for index,(server,client) in enumerate(chosen):
        control.selectedServer[index]=server.index
        control.selectedClient[index]=client.index
    def evidence(pair):
        server,client=pair
        return dict(server=plain(server),client=plain(client),four_tuple=list(tuple_key(client)),
                    server_identity=snapshots[(server.pid,server.tid)],client_identity=snapshots[(client.pid,client.tid)])
    return ([evidence(pair) for pair in chosen],[evidence(pair) for pair in pairs])


def expected_markers(control):
    result=[]
    for endpoint,records in [('server',control.serverThreads),('client',control.clientThreads)]:
        for index,item in enumerate(records):
            role='worker' if index<(4 if endpoint=='server' else 2) else 'main' if index==(4 if endpoint=='server' else 2) else 'logger'
            result.append(dict(endpoint=endpoint,role=role,worker=index,pid=item.pid,tid=item.tid,starttime=item.starttime))
    return result


def sample(args):
    output=Path(args.output).absolute()
    demand(output.parent==ROLE and re.fullmatch(r'run-[A-Za-z0-9_-]+',output.name) and output.is_dir() and not any(path.is_symlink() for path in (output,*output.parents)),'output outside exact admitted role run')
    demand(os.getuid()==1000 and os.getgid()==1000,'load identity must remain power')
    demand(args.connections in (8,32,64,128) and (args.warmup,args.duration) in ((1,3),(5,20)),'unapproved sample parameters')
    manifest=json.loads(Path(args.manifest).read_text())
    for name in ('server','client','summary'):
        item=manifest[name]
        demand(hashlib.sha256(Path(item['path']).read_bytes()).hexdigest()==item['sha256'],name+' sealed input drift')
    runtime=manifest['runtime']
    library=Path(runtime['library'])
    demand(str(library.parent)==runtime['library_directory']
           and hashlib.sha256(library.read_bytes()).hexdigest()==runtime['sha256'],
           'sealed LuaJIT runtime drift')
    os.umask(0o022)
    root=output/'fixture'
    root.mkdir()
    payload=root/'payload-1024.bin'
    payload.write_bytes(bytes(range(256))*4)
    demand(manifest['payload']['name']==payload.name and manifest['payload']['size']==payload.stat().st_size
           and manifest['payload']['sha256']==hashlib.sha256(payload.read_bytes()).hexdigest(),
           'original P3 payload bytes differ')
    control_path=output/'startup-control.bin'
    fd=os.open(control_path,os.O_RDWR|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
    os.ftruncate(fd,ctypes.sizeof(Control))
    mapping=mmap.mmap(fd,ctypes.sizeof(Control));os.close(fd)
    control=Control.from_buffer(mapping)
    demand((ctypes.sizeof(Control),ctypes.sizeof(Connection),ctypes.sizeof(Thread))==(16864,64,32),'native ABI mismatch')
    control.magic=b'S4CTRL02';control.version=2;control.bytes=ctypes.sizeof(Control)
    control.connections=args.connections;control.detailed=int(args.detailed)
    control.warmupNs=args.warmup*10**9;control.measurementNs=args.duration*10**9
    atomics=Atomics()
    channel=socket.socket(fileno=args.root_socket_fd)
    channel.settimeout(3)
    marker_fd=args.marker_fd
    env=os.environ.copy()
    env.update(LD_LIBRARY_PATH=runtime['library_directory'],LUA_PATH=runtime['lua_path'])
    env.update(HP_S4_CONTROL=str(control_path),HP_S4_OUTPUT=str(output),HP_S4_MARKER_FD=str(marker_fd),HP_S4_TRUSTED_SUMMARY=manifest['summary']['path'])
    processes=[];streams=[];cleanup_errors=[];forced=False
    result=dict(status='invalid',parameters=dict(connections=args.connections,warmup=args.warmup,duration=args.duration,detailed=args.detailed),
                route=dict(uid=os.getuid(),gid=os.getgid(),groups=os.getgroups(),affinity=sorted(os.sched_getaffinity(0)),nofile=list(resource.getrlimit(resource.RLIMIT_NOFILE)),
                           tmp={name:env.get(name) for name in ('TMPDIR','TMP','TEMP','XDG_CACHE_HOME')},identity=identity(os.getpid())),
                manifest=manifest,limits=['single fixed identity-selected subset; no automatic reconnect','raw first-write latency and original wrk corrected histogram are separate',
                                         'completion window is [measurement_start,measurement_end); crossing requests retained as boundaries'])
    def spawn(name,command):
        stdout=(output/(name+'.stdout')).open('wb');stderr=(output/(name+'.stderr')).open('wb');streams.extend([stdout,stderr])
        process=subprocess.Popen(command,stdout=stdout,stderr=stderr,env=env,pass_fds=(marker_fd,))
        record=identity(process.pid)
        processes.append((name,process,record))
        write_json(output/'processes.json',[dict(name=n,**r) for n,p,r in processes])
        return process
    def stop_for_deadline(number,frame):raise TimeoutError('power sample work deadline/signal')
    saved_handlers={sig:signal.signal(sig,stop_for_deadline) for sig in (signal.SIGALRM,signal.SIGTERM,signal.SIGINT)}
    signal.setitimer(signal.ITIMER_REAL,max(.001,args.work_deadline-time.monotonic()))
    try:
        result['resources_before_start']=resource_gate(output,'before-start')
        command=[manifest['server']['path'],'--threads','4','--idle-timeout-ms','30000','--keep-alive-timeout-ms','15000','--shutdown-timeout-ms','5000','--port','0','--root',str(root)]
        server=spawn('server',command);result['server_command']=command
        startup_deadline=min(time.monotonic()+3,args.work_deadline)
        port=None
        while time.monotonic()<startup_deadline:
            demand(server.poll() is None,'server ended during startup')
            match=re.search(r'listening on port (\d+)\.',(output/'server.stdout').read_text(errors='replace'))
            if match:port=int(match.group(1));break
            time.sleep(.005)
        demand(port,'listen port missing')
        command=[manifest['client']['path'],'-t2','-c'+str(args.connections),'--timeout','2s','--latency','-d',str(args.duration)+'s','-s',manifest['summary']['path'],f'http://127.0.0.1:{port}/{payload.name}']
        client=spawn('client',command);result['client_command']=command
        os.close(marker_fd);marker_fd=None
        thread_ready=connection_ready=False
        while time.monotonic()<startup_deadline:
            demand(server.poll() is None and client.poll() is None,'endpoint died during freeze')
            demand(not atomics.load(control,'abortRun'),'observer aborted startup')
            thread_ready=all(atomics.load(item,'ready')==1 for item in list(control.serverThreads)+list(control.clientThreads))
            connection_ready=all(atomics.load(control.serverConnections[index],'ready')==1 and atomics.load(control.clientConnections[index],'ready')==1 for index in range(args.connections))
            if thread_ready and connection_ready:break
            time.sleep(.001)
        demand(thread_ready and connection_ready,'startup metadata deadline')
        result['frozen_connections'],result['all_connection_pairs']=freeze(control,atomics)
        markers=expected_markers(control)
        result['startup_threads']=markers
        channel.send(json.dumps(dict(kind='ready',markers=markers)).encode())
        reply=json.loads(channel.recv(65536))
        expected={(item['endpoint'],item['role'],item['worker'],item['pid'],item['tid'],item['starttime']) for item in markers}
        received={(item['endpoint'],item['role'],item['worker'],item['pid'],item['namespace_tid'],item['starttime']) for item in reply.get('mappings',[])}
        demand(reply.get('kind')=='mapped' and len(reply.get('mappings',[]))==9 and received==expected,'kernel TID mapping incomplete')
        demand(len({item['kernel_tid'] for item in reply['mappings']})==9 and all(type(item['kernel_tid']) is int and item['kernel_tid']>0 for item in reply['mappings']),'invalid/duplicate kernel TID')
        for item in reply['mappings']:
            actual=identity(item['pid'],item['namespace_tid'])
            demand(actual['starttime']==item['starttime'] and actual['pid_namespace']==item['identity']['pid_namespace'],'mapped identity/namespace changed')
        result['kernel_mappings']=reply['mappings']
        maps_begin=time.monotonic_ns()
        client_pid=next(record['pid'] for name,process,record in processes if name=='client')
        loaded=[]
        status=library.stat()
        for line in (Path('/proc')/str(client_pid)/'maps').read_text().splitlines():
            fields=line.split(maxsplit=5)
            if len(fields)==6 and 'libluajit' in fields[5]:
                demand(fields[5]==str(library.resolve()) and int(fields[4])==status.st_ino,
                       'client loaded a different LuaJIT DSO')
                major,minor=(int(part,16) for part in fields[3].split(':'))
                demand((major,minor)==(os.major(status.st_dev),os.minor(status.st_dev)),
                       'client LuaJIT DSO device differs')
                loaded.append(line)
        demand(loaded,'client sealed LuaJIT not actually mapped')
        result['runtime_mapping']=dict(read_start_ns=maps_begin,read_end_ns=time.monotonic_ns(),
                                       library=runtime,actual_maps=loaded)
        controller_identity=identity(os.getpid())
        clock_evidence={}
        for endpoint in ('server','client'):
            evidence=json.loads((output/f'{endpoint}-clock.json').read_text())
            main=next(item for item in reply['mappings'] if item['endpoint']==endpoint and item['role']=='main')
            demand(evidence['pid']==main['pid'] and evidence['clock']=='CLOCK_MONOTONIC'
                   and evidence['unit']=='ns' and evidence['resolution_ns']>0 and evidence['clock_pair_count']==32,
                   'endpoint clock schema/identity invalid')
            demand(evidence['boot_id']==main['identity']['boot_id']==controller_identity['boot_id']
                   and evidence['time_namespace']==main['identity']['time_namespace']==controller_identity['time_namespace'],
                   'endpoint/controller boot or time namespace differs')
            for field in ('boot_read_ns','namespace_read_ns','resolution_read_ns'):
                demand(len(evidence[field])==2 and 0<evidence[field][0]<=evidence[field][1]<time.monotonic_ns(),
                       'clock field read interval invalid')
            demand(0<=evidence['clock_pair_min_ns']<=evidence['clock_pair_max_ns'],'clock overhead evidence invalid')
            clock_evidence[endpoint]=evidence
        result['endpoint_clock_evidence']=clock_evidence
        result['resources_after_prefault_before_go']=resource_gate(output,'after-prefault-before-go')
        # File descriptions are closed in both loads before GO; numeric descriptors may be reused.
        for endpoint in ('server','client'):
            pid=next(r['pid'] for name,p,r in processes if name==endpoint)
            links=[]
            for path in (Path('/proc')/str(pid)/'fd').iterdir():
                try:links.append(os.readlink(path))
                except FileNotFoundError:pass
            demand(not any(link.endswith('/trace_marker') for link in links),'inherited marker FD still open')
        result['frozen_before_go_ns']=time.monotonic_ns()
        control.startNs=time.monotonic_ns()+20_000_000
        result['recording_start_ns']=control.startNs
        result['measurement_start_ns']=control.startNs+control.warmupNs
        result['measurement_end_ns']=result['measurement_start_ns']+control.measurementNs
        atomics.store(control,'go',1)
        while client.poll() is None:
            demand(time.monotonic()<args.work_deadline,'power work deadline reached')
            demand(server.poll() is None,'server died during sample')
            demand(not atomics.load(control,'abortRun'),'observer overflow/invalid')
            time.sleep(.005)
        demand(client.returncode==0 and not atomics.load(control,'abortRun'),'client invalid')
        server.send_signal(signal.SIGTERM)
        demand(server.wait(timeout=max(.001,min(5.5,args.work_deadline-time.monotonic())))==0,'server stop invalid')
        result['recording_end_ns']=time.monotonic_ns()
        result['final_threads']=[dict(endpoint=e,**plain(t)) for e,items in [('server',control.serverThreads),('client',control.clientThreads)] for t in items]
        result['connection_end_states']=[dict(endpoint=e,**plain(item)) for e,items in [('server',control.serverConnections),('client',control.clientConnections)] for item in list(items)[:args.connections]]
        demand(all(item['stoppedNs'] for item in result['final_threads'] if item['worker']<(4 if item['endpoint']=='server' else 2)),'writer stop missing')
        result['status']='valid'
    except BaseException as error:
        result['error']=repr(error)
    finally:
        signal.setitimer(signal.ITIMER_REAL,0)
        for sig in (signal.SIGTERM,signal.SIGINT):signal.signal(sig,signal.SIG_IGN)
        for name,process,record in reversed(processes):
            if process.poll() is None and alive(record):process.terminate()
        deadline=min(time.monotonic()+7,args.total_deadline-.4)
        while time.monotonic()<deadline and any(process.poll() is None for name,process,record in processes):time.sleep(.005)
        for name,process,record in processes:
            if process.poll() is None:
                forced=True
                if alive(record):process.kill()
            try:process.wait(timeout=max(.001,min(.1,args.total_deadline-time.monotonic()-.2)))
            except BaseException as error:cleanup_errors.append(name+':'+repr(error))
        result['cleanup']=dict(forced=forced,errors=cleanup_errors,remaining=[name for name,p,r in processes if p.poll() is None])
        if forced or cleanup_errors or result['cleanup']['remaining']:result['status']='invalid'
        if marker_fd is not None:os.close(marker_fd)
        channel.close()
        for stream in streams:stream.close()
        write_json(output/'sample.json',result)
        del control
        mapping.close()
        for sig,handler in saved_handlers.items():signal.signal(sig,handler)
    return 0 if result['status']=='valid' else 1


if __name__=='__main__':
    import json
    parser=argparse.ArgumentParser()
    parser.add_argument('--output',required=True);parser.add_argument('--manifest',required=True)
    parser.add_argument('--root-socket-fd',type=int,required=True);parser.add_argument('--marker-fd',type=int,required=True)
    parser.add_argument('--connections',type=int,required=True);parser.add_argument('--warmup',type=int,required=True);parser.add_argument('--duration',type=int,required=True)
    parser.add_argument('--detailed',action='store_true')
    parser.add_argument('--total-deadline',type=float,required=True);parser.add_argument('--work-deadline',type=float,required=True)
    raise SystemExit(sample(parser.parse_args()))
