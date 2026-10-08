"""Private trace_marker startup mapping only; no system events or perf recording."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import platform
import pwd
import re
import shutil
import signal
import socket
import subprocess
import time
import uuid
from wire_types import identity, write_json


def demand(value, message):
    if not value:raise RuntimeError(message)


def task_directory(task_tmp):
    path=task_tmp/('hp-s4-marker-'+uuid.uuid4().hex)
    previous=os.umask(0)
    try:path.mkdir(mode=0o755)
    finally:os.umask(previous)
    demand(path.lstat().st_mode & 0o777==0o755,'owned backing directory mode mismatch')
    return path


def alive(record):
    try:
        item=identity(record['pid'])
        return item['starttime']==record['starttime'] and item['state']!='Z'
    except FileNotFoundError:return False


def owned(root_pid, session):
    found=[]
    for path in Path('/proc').iterdir():
        if not path.name.isdecimal():continue
        try:
            item=identity(int(path.name))
            if item['pid']!=root_pid and (item['session']==session or item['ppid']==root_pid):found.append(item)
        except (FileNotFoundError,ProcessLookupError):pass
    return found


def mount_present(path):
    return any(line.split()[4]==str(path) for line in Path('/proc/self/mountinfo').read_text().splitlines())


def run(args):
    demand(os.getuid()==0 and platform.machine()=='x86_64','native WSL root x86_64 required')
    demand(args.seconds in (20,45),'unapproved marker deadline')
    role=Path(__file__).resolve().parents[2]/'.cache/v0.5.1-s4/builder'
    output=Path(args.output).absolute()
    demand(output.parent==role and re.fullmatch(r'run-[A-Za-z0-9_-]+',output.name) and output.is_dir() and not any(path.is_symlink() for path in (output,*output.parents)),'not admitted exact run root')
    root_identity=identity(os.getpid())
    started=getattr(args,'started',time.monotonic())
    total_deadline=started+args.seconds
    work_deadline=total_deadline-7
    summary=dict(status='invalid',root_identity=root_identity,start_monotonic=started,system_events_enabled=False,
                 limits=['marker records identify only this sample own startup threads','private instance, no syscall/scheduler/perf/host events'])
    runtime=json.loads(Path(args.manifest).read_text())['runtime']
    sealed_tools=json.loads(Path(args.manifest).read_text())['tools']
    for tool in ('root_marker_launcher_v2.py','observed_sample_v2.py','wire_types.py','decode_v2.py'):
        item=sealed_tools[tool]
        demand(Path(item['path']).resolve()==Path(__file__).with_name(tool).resolve() and hashlib.sha256(Path(item['path']).read_bytes()).hexdigest()==item['sha256'],'sealed tool drift: '+tool)
    demand(os.environ.get('LD_LIBRARY_PATH')==runtime['library_directory'] and os.environ.get('LUA_PATH')==runtime['lua_path'],
           'root sealed runtime environment mismatch')
    demand(hashlib.sha256(Path(runtime['library']).read_bytes()).hexdigest()==runtime['sha256'],'root runtime library drift')
    summary['runtime']=runtime
    cleanup_errors=[];forced=False;driver=None;driver_record=None;session=None;marker_fd=None;channel=None;peer=None;instance=None;mount_root=None
    saved_handlers={}
    def timeout(signum,frame):raise TimeoutError('marker launcher deadline/signal')
    for sig in (signal.SIGALRM,signal.SIGTERM,signal.SIGINT):saved_handlers[sig]=signal.signal(sig,timeout)
    signal.setitimer(signal.ITIMER_REAL,max(.001,work_deadline-time.monotonic()))
    libc=ctypes.CDLL(None,use_errno=True)
    libc.prctl.argtypes=[ctypes.c_int,ctypes.c_ulong,ctypes.c_ulong,ctypes.c_ulong,ctypes.c_ulong]
    try:
        demand(libc.prctl(36,1,0,0,0)==0,'subreaper failed')
        summary['mount_namespace_before']=os.readlink('/proc/self/ns/mnt')
        demand(libc.unshare(0x20000)==0,'private mount namespace failed errno='+str(ctypes.get_errno()))
        subprocess.run(['/usr/bin/mount','--make-rprivate','/'],check=True,timeout=1)
        task_tmp=Path(os.environ['TMPDIR']).resolve()
        demand(task_tmp==Path(__file__).resolve().parents[2]/'.cache/v0.5.1-s4/builder/tmp','task TMP route mismatch')
        mount_root=task_directory(task_tmp)
        instance=mount_root/'instances'/('s4-'+uuid.uuid4().hex)
        write_json(output/'marker-resource-plan.json',dict(root_identity=root_identity,
                   mount_directory=str(mount_root),instance=str(instance),total_deadline=total_deadline))
        summary['mount_directory']=str(mount_root)
        summary['mount_namespace_private']=os.readlink('/proc/self/ns/mnt')
        demand(not mount_present(mount_root),'mount unexpectedly exists')
        subprocess.run(['/usr/bin/mount','-t','tracefs','tracefs',str(mount_root)],check=True,timeout=1)
        demand(mount_present(mount_root),'tracefs mount did not complete')
        instance.mkdir()
        for name,value in [('tracing_on','0'),('current_tracer','nop'),('events/enable','0'),('trace_clock','mono')]:
            (instance/name).write_text(value)
        demand((instance/'current_tracer').read_text().strip()=='nop','tracer readback')
        demand((instance/'events/enable').read_text().strip()=='0','events not disabled')
        demand('[mono]' in (instance/'trace_clock').read_text(),'mono clock readback')
        (instance/'buffer_size_kb').write_text('64')
        summary['trace_setup']=dict(clock=(instance/'trace_clock').read_text(),buffer_kb=(instance/'buffer_size_kb').read_text(),
                                   current_tracer=(instance/'current_tracer').read_text(),events_enable=(instance/'events/enable').read_text())
        (instance/'trace').write_text('')
        (instance/'tracing_on').write_text('1')
        marker_fd=os.open(instance/'trace_marker',os.O_WRONLY|os.O_CLOEXEC)
        channel,peer=socket.socketpair(socket.AF_UNIX,socket.SOCK_SEQPACKET)
        channel.settimeout(4)
        user=pwd.getpwnam('power')
        def power_identity():
            os.initgroups(user.pw_name,user.pw_gid);os.setgid(user.pw_gid);os.setuid(user.pw_uid)
        command=['/usr/bin/python3',str(Path(__file__).with_name('observed_sample_v2.py')),'--output',str(output),'--manifest',args.manifest,
                 '--root-socket-fd',str(peer.fileno()),'--marker-fd',str(marker_fd),'--connections',str(args.connections),
                 '--warmup',str(args.warmup),'--duration',str(args.duration),'--total-deadline',str(total_deadline),'--work-deadline',str(work_deadline)]
        if args.detailed:command.append('--detailed')
        with (output/'driver.stdout').open('wb') as stdout,(output/'driver.stderr').open('wb') as stderr:
            driver=subprocess.Popen(command,stdout=stdout,stderr=stderr,pass_fds=(peer.fileno(),marker_fd),preexec_fn=power_identity,start_new_session=True)
            driver_record=identity(driver.pid);session=driver_record['session'];summary['driver_identity']=driver_record;peer.close();peer=None
            write_json(output/'marker-driver-identity.json',driver_record)
            request=json.loads(channel.recv(65536))
            demand(request.get('kind')=='ready' and len(request.get('markers',[]))==9,'marker handshake malformed')
            (instance/'tracing_on').write_text('0')
            raw=(instance/'trace').read_bytes()
            demand(len(raw)<=65536,'unexpected marker trace capacity')
            (output/'startup-markers.stdout').write_bytes(raw)
            summary['marker_sha256']=hashlib.sha256(raw).hexdigest()
            cpu_stats=[]
            for stats_path in sorted((instance/'per_cpu').glob('cpu*/stats')):
                text=stats_path.read_text()
                fields={key.strip():int(value.strip()) for key,value in
                        (line.split(':',1) for line in text.splitlines() if ':' in line)
                        if value.strip().isdigit()}
                required={'entries','overrun','commit overrun','dropped events'}
                demand(required<=fields.keys(),'marker CPU loss fields missing')
                cpu_stats.append(dict(cpu=stats_path.parent.name,fields=fields,raw=text))
            demand(cpu_stats,'marker CPU stats missing')
            summary['marker_cpu_stats']=cpu_stats
            summary['marker_lost_events']=sum(item['fields'][key] for item in cpu_stats
                                              for key in ('overrun','commit overrun','dropped events'))
            write_json(output/'startup-marker-stats.json',cpu_stats)
            demand(summary['marker_lost_events']==0,'startup marker event loss')
            mappings=[];seen=set()
            expected={(item['endpoint'],item['role'],item['worker'],item['pid'],item['tid'],item['starttime']) for item in request['markers']}
            demand(len(expected)==9,'duplicate expected marker identity')
            expression=re.compile(r'-(\d+)\s+\[\d+\].*tracing_mark_write:\s+S4MARK (server|client) (worker|main|logger) (\d+) (\d+) (\d+) (\d+)\s*$')
            for line in raw.decode().splitlines():
                if not line.strip() or line.startswith('#'):continue
                match=expression.search(line)
                demand(match is not None,'unexpected marker line')
                kernel_tid,endpoint,role,worker,pid,tid,starttime=match.groups()
                key=(endpoint,role,int(worker),int(pid),int(tid),int(starttime))
                demand(key in expected and key not in seen,'marker unmatched/duplicate')
                actual=identity(int(pid),int(tid));demand(actual['starttime']==int(starttime),'marker live identity drift')
                mappings.append(dict(endpoint=endpoint,role=role,worker=int(worker),pid=int(pid),namespace_tid=int(tid),starttime=int(starttime),kernel_tid=int(kernel_tid),identity=actual))
                seen.add(key)
            demand(seen==expected and len({item['kernel_tid'] for item in mappings})==9,'kernel TID mapping incomplete')
            os.close(marker_fd);marker_fd=None
            summary['mappings']=mappings
            channel.send(json.dumps(dict(kind='mapped',mappings=mappings)).encode())
            while driver.poll() is None:
                summary['owned_identities']=owned(os.getpid(),session)
                time.sleep(.01)
            demand(driver.returncode==0,'power driver invalid')
            sample=json.loads((output/'sample.json').read_text())
            demand(sample['status']=='valid' and not sample['cleanup']['forced'] and not sample['cleanup']['errors'] and not sample['cleanup']['remaining'],'sample/cleanup invalid')
            summary['status']='valid'
    except BaseException as error:
        summary['error']=repr(error)
    finally:
        signal.setitimer(signal.ITIMER_REAL,0)
        for sig in (signal.SIGTERM,signal.SIGINT):signal.signal(sig,signal.SIG_IGN)
        if instance is not None and instance.exists():
            try:(instance/'tracing_on').write_text('0');(instance/'events/enable').write_text('0')
            except BaseException as error:cleanup_errors.append('disable:'+repr(error))
        candidates=[item for item in owned(os.getpid(),session) if item['starttime']>=root_identity['starttime']]
        for item in candidates:
            if alive(item):
                try:os.kill(item['pid'],signal.SIGTERM)
                except ProcessLookupError:pass
        deadline=min(time.monotonic()+7,total_deadline-.8)
        while time.monotonic()<deadline:
            if driver is not None:driver.poll()
            known={(item['pid'],item['starttime']) for item in candidates}
            for item in owned(os.getpid(),session):
                if item['starttime']>=root_identity['starttime'] and (item['pid'],item['starttime']) not in known:
                    candidates.append(item)
                    if alive(item):
                        try:os.kill(item['pid'],signal.SIGTERM)
                        except ProcessLookupError:pass
            while True:
                try:
                    pid,status=os.waitpid(-1,os.WNOHANG)
                    if not pid:break
                    summary.setdefault('adopted_reaped',[]).append(dict(pid=pid,status=status))
                except ChildProcessError:break
            remaining=[item for item in candidates if alive(item)]
            if not remaining:break
            time.sleep(.01)
        for item in candidates:
            if alive(item):
                forced=True
                try:os.kill(item['pid'],signal.SIGKILL)
                except ProcessLookupError:pass
        if driver is not None:
            try:driver.wait(timeout=max(.001,min(.2,total_deadline-time.monotonic()-.6)))
            except BaseException as error:cleanup_errors.append('driver reap:'+repr(error))
        while True:
            try:
                pid,status=os.waitpid(-1,os.WNOHANG)
                if pid==0:break
            except ChildProcessError:break
        if marker_fd is not None:os.close(marker_fd)
        if channel is not None:channel.close()
        if peer is not None:peer.close()
        removed=False;unmounted=False
        if instance is not None:
            try:instance.rmdir();removed=not instance.exists()
            except BaseException as error:cleanup_errors.append('instance remove:'+repr(error))
        if mount_root is not None:
            try:
                if mount_present(mount_root):subprocess.run(['/usr/bin/umount',str(mount_root)],check=True,timeout=.5)
                unmounted=not mount_present(mount_root)
                summary['task_mount_retained_file_bytes']=sum(path.lstat().st_size for path in mount_root.iterdir() if path.is_file())
                demand(summary['task_mount_retained_file_bytes']==0,'unexpected files in owned mount backing directory')
                mount_root.rmdir()
            except BaseException as error:cleanup_errors.append('unmount:'+repr(error))
        remaining=[item for item in owned(os.getpid(),session) if item['starttime']>=root_identity['starttime']]
        summary['cleanup']=dict(forced=forced,errors=cleanup_errors,remaining=remaining,instance_removed=removed,mount_restored=unmounted,driver_reaped=driver is None or driver.returncode is not None)
        if cleanup_errors or forced or remaining or not removed or not unmounted:summary['status']='invalid'
        summary['wall_seconds']=time.monotonic()-started
        write_json(output/'marker-launcher.json',summary)
        for sig,handler in saved_handlers.items():signal.signal(sig,handler)
    return 0 if summary['status']=='valid' else 1


def supervise(args):
    """Independent parent clock bounds the worker even when its filesystem walk stalls."""
    output=Path(args.output)
    role=Path(__file__).resolve().parents[2]/'.cache/v0.5.1-s4/builder'
    demand(output.is_absolute() and output.parent==role and re.fullmatch(r'run-[A-Za-z0-9_-]+',output.name)
           and output.is_dir() and not any(path.is_symlink() for path in (output,*output.parents)),
           'supervisor requires admitted exact run root')
    ledger=json.loads((role/'ledger.json').read_text())
    records=[item for item in ledger['runs'] if item['run_id']==output.name]
    demand(len(records)==1 and records[0]['status']=='running' and records[0]['kind'] in ('smoke','staircase','observed_abba')
           and records[0]['reserved_seconds']==args.seconds and records[0]['output']==str(output),
           'root deadline must bind the outer admitted ledger reservation')
    args.started=records[0]['start_monotonic']
    demand(0<=time.monotonic()-args.started<args.seconds-7,'outer work window already expired or clock mismatch')
    deadline=args.started+args.seconds
    libc=ctypes.CDLL(None,use_errno=True)
    demand(libc.prctl(36,1,0,0,0)==0,'supervisor subreaper failed')
    supervisor_identity=identity(os.getpid())
    interrupted=[]
    saved={sig:signal.signal(sig,lambda number,frame:interrupted.append(number))
           for sig in (signal.SIGTERM,signal.SIGINT)}
    child=os.fork()
    if child==0:
        try:os._exit(run(args))
        except BaseException:os._exit(1)
    record=identity(child);namespace_fd=None;forced=False;errors=[];reaped=[];status=None;session=None
    try:
        while time.monotonic()<deadline-.8 and not interrupted:
            pid,code=os.waitpid(child,os.WNOHANG)
            if pid:status=code;reaped.append(child);break
            if namespace_fd is None and (output/'marker-resource-plan.json').exists():
                namespace_fd=os.open(f'/proc/{child}/ns/mnt',os.O_RDONLY|os.O_CLOEXEC)
            time.sleep(.01)
        if status is None:
            forced=True
            # Identity is checked immediately before every signal; no unrelated PID is targeted.
            driver_path=output/'marker-driver-identity.json'
            session=json.loads(driver_path.read_text())['session'] if driver_path.exists() else None
            candidates=[item for item in owned(os.getpid(),session)
                        if item['starttime']>=supervisor_identity['starttime']]+[record]
            for item in candidates:
                if alive(item):os.kill(item['pid'],signal.SIGKILL)
            if alive(record):os.kill(child,signal.SIGKILL)
            if namespace_fd is not None:
                demand(libc.setns(namespace_fd,0x20000)==0,'supervisor enter owned mount namespace failed')
                plan=json.loads((output/'marker-resource-plan.json').read_text())
                demand(plan['root_identity']['pid']==child and plan['root_identity']['starttime']==record['starttime'],'resource owner mismatch')
                instance=Path(plan['instance']);mount_root=Path(plan['mount_directory'])
                if instance.exists():
                    (instance/'tracing_on').write_text('0');(instance/'events/enable').write_text('0');instance.rmdir()
                if mount_present(mount_root):subprocess.run(['/usr/bin/umount',str(mount_root)],check=True,timeout=max(.001,deadline-time.monotonic()-.2))
                if mount_root.exists():mount_root.rmdir()
    except BaseException as error:errors.append(repr(error))
    finally:
        # Every exit path owns cleanup, including an early worker failure or metadata error.
        try:
            driver_path=output/'marker-driver-identity.json'
            if driver_path.exists():session=json.loads(driver_path.read_text())['session']
            candidates=[item for item in owned(os.getpid(),session)
                        if item['starttime']>=supervisor_identity['starttime']]+[record]
            for item in candidates:
                if alive(item):
                    forced=True;os.kill(item['pid'],signal.SIGKILL)
            if alive(record):forced=True;os.kill(child,signal.SIGKILL)
        except BaseException as error:errors.append('final owned cleanup:'+repr(error))
        restored=False
        try:
            if namespace_fd is not None:
                demand(libc.setns(namespace_fd,0x20000)==0,'final owned namespace enter failed')
                plan=json.loads((output/'marker-resource-plan.json').read_text())
                demand(plan['root_identity']['pid']==child and plan['root_identity']['starttime']==record['starttime'],'final resource owner mismatch')
                instance=Path(plan['instance']);mount_root=Path(plan['mount_directory'])
                if instance.exists():
                    (instance/'tracing_on').write_text('0');(instance/'events/enable').write_text('0');instance.rmdir()
                if mount_present(mount_root):subprocess.run(['/usr/bin/umount',str(mount_root)],check=True,timeout=max(.001,deadline-time.monotonic()-.2))
                if mount_root.exists():mount_root.rmdir()
                restored=not instance.exists() and not mount_present(mount_root) and not mount_root.exists()
            else:
                worker_summary=json.loads((output/'marker-launcher.json').read_text())
                restored=worker_summary['cleanup']['instance_removed'] and worker_summary['cleanup']['mount_restored']
        except BaseException as error:errors.append('final resource cleanup:'+repr(error))
        if namespace_fd is not None:os.close(namespace_fd)
        while time.monotonic()<deadline-.1:
            try:
                pid,code=os.waitpid(-1,os.WNOHANG)
                if pid:reaped.append(pid);continue
                adopted=[item for item in owned(os.getpid(),session)
                         if item['starttime']>=supervisor_identity['starttime']]
                for item in adopted:
                    if alive(item):forced=True;os.kill(item['pid'],signal.SIGKILL)
                if not adopted and not alive(record):break
            except ChildProcessError:break
            time.sleep(.005)
        remaining=[item for item in owned(os.getpid(),session)
                   if item['starttime']>=supervisor_identity['starttime']]
        try:
            worker_summary=json.loads((output/'marker-launcher.json').read_text())
            demand(worker_summary['status']=='valid' and not worker_summary['cleanup']['errors']
                   and not worker_summary['cleanup']['forced'] and not worker_summary['cleanup']['remaining'],
                   'worker cleanup/status invalid')
        except BaseException as error:errors.append('worker final validation:'+repr(error))
        if remaining or not restored:errors.append('owned processes/resources remain or restoration unknown')
        write_json(output/'marker-supervisor.json',dict(forced=forced,errors=errors,reaped=reaped,
                   remaining=remaining,resources_restored=restored,
                   interrupted=interrupted,worker_identity=record,worker_wait_status=status,wall_seconds=time.monotonic()-args.started))
        for sig,handler in saved.items():signal.signal(sig,handler)
    return 1 if forced or errors or status!=0 else 0


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--output',required=True);parser.add_argument('--manifest',required=True)
    parser.add_argument('--seconds',type=int,required=True);parser.add_argument('--connections',type=int,required=True)
    parser.add_argument('--warmup',type=int,required=True);parser.add_argument('--duration',type=int,required=True);parser.add_argument('--detailed',action='store_true')
    raise SystemExit(supervise(parser.parse_args()))
