from cleanup_v11 import emit_error
from m3_contract_v2 import admit_observed
from m3_environment_v2 import publish as publish_environment
from inner_identity_v11 import CleanupIdentity,required_identity,cleanup_step,cleanup_evidence
from proc_identity_v11 import error_record
from cleanup_v11 import emit_error
from control_protocol_v3 import encode_message, receive_message, read_published_failure
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
from wire_types import identity as stable_identity, write_json


def identity(pid,tid=None):
    return required_identity(stable_identity,pid,tid)


def demand(value, message):
    if not value:raise RuntimeError(message)


def task_directory(task_tmp):
    path=task_tmp/('hp-s4-marker-'+uuid.uuid4().hex)
    previous=os.umask(0)
    try:path.mkdir(mode=0o755)
    finally:os.umask(previous)
    demand(path.lstat().st_mode & 0o777==0o755,'owned backing directory mode mismatch')
    return path


identity_cleanup=CleanupIdentity()
alive=identity_cleanup.alive
owned=identity_cleanup.owned


def mount_present(path):
    return any(line.split()[4]==str(path) for line in Path('/proc/self/mountinfo').read_text().splitlines())


def publish_worker_failure(args,error):
    """A first cause for exceptions before run() can publish its own summary."""
    pid=os.getpid();owner=getattr(args,'worker_identity',None);errors=[]
    if owner is None:
        try:owner=identity(pid)
        except BaseException as acquisition_error:errors.append(error_record(acquisition_error,'marker.worker.identity',pid))
    role_root=Path(__file__).resolve().parents[2]/'.cache/v0.5.1-s4'/args.role
    tools_root=Path(__file__).resolve().parents[2]/'.cache/v0.5-s4/tools/root'
    allowed=dict(TMPDIR=str(role_root/'tmp'),TMP=str(role_root/'tmp'),TEMP=str(role_root/'tmp'),XDG_CACHE_HOME=str(role_root/'cache'),PYTHONDONTWRITEBYTECODE='1',LD_LIBRARY_PATH=str(tools_root/'usr/lib/x86_64-linux-gnu'),LUA_PATH=str(tools_root/'usr/share/luajit-2.1/?.lua')+';;')
    environment={name:dict(present=os.environ.get(name) is not None,matches_expected=os.environ.get(name)==expected,value=expected if os.environ.get(name)==expected else None) for name,expected in allowed.items()}
    phase=getattr(args,'worker_phase','unknown')
    not_created=getattr(args,'private_resources_entered',True) is False and phase in ('admission','tool-verification','runtime-verification','preparation')
    value=dict(run_id=Path(args.output).name,worker_identity=owner,first_error=error_record(error,'marker.worker.startup',getattr(error,'target_pid',None) or pid),worker_phase=phase,resource_state='not_created' if not_created else getattr(args,'private_resource_state','unknown'),environment=environment,evidence_errors=errors)
    cleanup_evidence(Path(args.output)/'marker-worker-failure.json',value,write_json,errors)


def run(args):
    args.worker_phase='admission';args.private_resources_entered=False
    demand(os.getuid()==0 and platform.machine()=='x86_64','native WSL root x86_64 required')
    demand(args.seconds==45,'original M3 outer deadline required')
    admit_observed(args)
    role=Path(__file__).resolve().parents[2]/'.cache/v0.5.1-s4'/args.role
    output=Path(args.output).absolute()
    demand(output.parent==role and re.fullmatch(r'run-[A-Za-z0-9_-]+',output.name) and output.is_dir() and not any(path.is_symlink() for path in (output,*output.parents)),'not admitted exact run root')
    root_identity=identity(os.getpid());args.worker_identity=root_identity
    args.worker_phase='tool-verification'
    started=getattr(args,'started',time.monotonic())
    total_deadline=started+args.seconds
    work_deadline=total_deadline-7
    summary=dict(status='invalid',resource_state='not_created',root_identity=root_identity,start_monotonic=started,system_events_enabled=False,
                 limits=['marker records identify only this sample own startup threads','private instance, no syscall/scheduler/perf/host events'])
    runtime=json.loads(Path(args.manifest).read_text())['runtime']
    sealed_tools=json.loads(Path(args.manifest).read_text())['tools']
    for tool in ('root_marker_launcher_m3_v3.py','observed_sample_m3_v2.py','wire_types_v3.py','control_protocol_v3.py','decode_v3.py','proc_identity_v11.py','cleanup_v11.py','inner_identity_v11.py','localize_v12.py','watchdog_v11.py','m3_contract_v2.py','m3_environment_v2.py','m3_metrics.py'):
        item=sealed_tools[tool]
        demand(Path(item['path']).resolve()==Path(__file__).with_name(tool).resolve() and hashlib.sha256(Path(item['path']).read_bytes()).hexdigest()==item['sha256'],'sealed tool drift: '+tool)
    args.worker_phase='runtime-verification'
    demand(os.environ.get('LD_LIBRARY_PATH')==runtime['library_directory'] and os.environ.get('LUA_PATH')==runtime['lua_path'],
           'root sealed runtime environment mismatch')
    demand(hashlib.sha256(Path(runtime['library']).read_bytes()).hexdigest()==runtime['sha256'],'root runtime library drift')
    summary['runtime']=runtime
    manifest=json.loads(Path(args.manifest).read_text())
    summary['environment_evidence']=publish_environment(output,args.role,'root-before-resources',manifest,root_identity,0,0)
    args.worker_phase='preparation'
    cleanup_errors=[];forced=False;driver=None;driver_record=None;session=None;marker_fd=None;channel=None;peer=None;instance=None;mount_root=None
    saved_handlers={}
    def timeout(signum,frame):raise TimeoutError('marker launcher deadline/signal')
    for sig in (signal.SIGALRM,signal.SIGTERM,signal.SIGINT):saved_handlers[sig]=signal.signal(sig,timeout)
    signal.setitimer(signal.ITIMER_REAL,max(0,work_deadline-time.monotonic()))
    libc=ctypes.CDLL(None,use_errno=True)
    libc.prctl.argtypes=[ctypes.c_int,ctypes.c_ulong,ctypes.c_ulong,ctypes.c_ulong,ctypes.c_ulong]
    try:
        demand(libc.prctl(36,1,0,0,0)==0,'subreaper failed')
        summary['mount_namespace_before']=os.readlink('/proc/self/ns/mnt')
        args.worker_phase='resource-setup';args.private_resources_entered=True
        demand(libc.unshare(0x20000)==0,'private mount namespace failed errno='+str(ctypes.get_errno()))
        args.private_resource_state='created';summary['resource_state']='created'
        subprocess.run(['/usr/bin/mount','--make-rprivate','/'],check=True,timeout=1)
        task_tmp=Path(os.environ['TMPDIR']).resolve()
        demand(task_tmp==Path(__file__).resolve().parents[2]/'.cache/v0.5.1-s4'/args.role/'tmp','task TMP route mismatch')
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
        command=['/usr/bin/python3',str(Path(__file__).with_name('observed_sample_m3_v2.py')),'--role',args.role,'--output',str(output),'--manifest',args.manifest,
                 '--root-socket-fd',str(peer.fileno()),'--marker-fd',str(marker_fd),'--connections',str(args.connections),
                 '--identity-scope','kernel-mapped','--requests-per-connection',str(args.requests_per_connection),'--warmup',str(args.warmup),'--duration',str(args.duration),'--total-deadline',str(total_deadline),'--work-deadline',str(work_deadline)]
        if args.detailed:command.append('--detailed')
        with (output/'driver.stdout').open('wb') as stdout,(output/'driver.stderr').open('wb') as stderr:
            driver=subprocess.Popen(command,stdout=stdout,stderr=stderr,pass_fds=(peer.fileno(),marker_fd),preexec_fn=power_identity,start_new_session=True)
            driver_record=identity(driver.pid);session=driver_record['session'];summary['driver_identity']=driver_record;peer.close();peer=None
            write_json(output/'marker-driver-identity.json',driver_record)
            request=receive_message(channel,output.name,'ready',work_deadline,set())
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
            channel.send(encode_message(output.name,'running','go',mappings=mappings))
            while driver.poll() is None:
                summary['owned_identities']=owned(os.getpid(),session)
                time.sleep(.01)
            demand(driver.returncode==0,'power driver invalid')
            sample=json.loads((output/'sample.json').read_text())
            demand(sample['status']=='valid' and not sample['cleanup']['forced'] and not sample['cleanup']['errors'] and not sample['cleanup']['remaining'],'sample/cleanup invalid')
            summary['status']='valid'
    except BaseException as error:
        summary['work_failure']=error_record(error,'inner.work')
        summary['error']=repr(error)
        summary['first_failure']=read_published_failure(output/'startup-control.bin',total_deadline)
    finally:
        identity_cleanup.cleaning=True
        signal.setitimer(signal.ITIMER_REAL,0)
        for sig in (signal.SIGTERM,signal.SIGINT):signal.signal(sig,signal.SIG_IGN)
        if instance is not None and cleanup_step(cleanup_errors,'instance_exists',instance.exists):
            try:(instance/'tracing_on').write_text('0');(instance/'events/enable').write_text('0')
            except BaseException as error:cleanup_errors.append('disable:'+repr(error))
        candidates=[item for item in owned(os.getpid(),session) if item['starttime']>=root_identity['starttime']]
        for item in candidates:
            if alive(item):
                try:cleanup_step(cleanup_errors,'term',lambda item=item:os.kill(item['pid'],signal.SIGTERM))
                except ProcessLookupError:pass
        deadline=min(time.monotonic()+7,total_deadline-.8)
        while time.monotonic()<deadline:
            if driver is not None:driver.poll()
            known={(item['pid'],item['starttime']) for item in candidates}
            for item in owned(os.getpid(),session):
                if item['starttime']>=root_identity['starttime'] and (item['pid'],item['starttime']) not in known:
                    candidates.append(item)
                    if alive(item):
                        try:cleanup_step(cleanup_errors,'term',lambda item=item:os.kill(item['pid'],signal.SIGTERM))
                        except ProcessLookupError:pass
            while True:
                try:
                    pid,status=os.waitpid(-1,os.WNOHANG)
                    if not pid:break
                    summary.setdefault('adopted_reaped',[]).append(dict(pid=pid,status=status))
                except ChildProcessError:break
                except BaseException as error:cleanup_errors.append(error_record(error,'inner.cleanup.adopted_wait'));break
            remaining=[item for item in candidates if alive(item)]
            if not remaining:break
            time.sleep(.01)
        for item in candidates:
            if alive(item):
                forced=True
                try:cleanup_step(cleanup_errors,'kill',lambda item=item:os.kill(item['pid'],signal.SIGKILL))
                except ProcessLookupError:pass
        if driver is not None:
            try:driver.wait(timeout=max(0,min(.2,total_deadline-time.monotonic()-.6)))
            except BaseException as error:cleanup_errors.append('driver reap:'+repr(error))
        while True:
            try:
                pid,status=os.waitpid(-1,os.WNOHANG)
                if pid==0:break
            except ChildProcessError:break
            except BaseException as error:cleanup_errors.append(error_record(error,'inner.cleanup.adopted_wait'));break
        if marker_fd is not None:cleanup_step(cleanup_errors,'marker_fd',lambda:os.close(marker_fd))
        if channel is not None:cleanup_step(cleanup_errors,'channel',channel.close)
        if peer is not None:cleanup_step(cleanup_errors,'peer',peer.close)
        removed=False;unmounted=False
        if instance is not None:
            try:instance.rmdir();removed=not instance.exists()
            except BaseException as error:cleanup_errors.append('instance remove:'+repr(error))
        if mount_root is not None:
            try:
                if mount_present(mount_root):subprocess.run(['/usr/bin/umount',str(mount_root)],check=True,timeout=max(0,min(.5,total_deadline-time.monotonic())))
                unmounted=not mount_present(mount_root)
                summary['task_mount_retained_file_bytes']=sum(path.lstat().st_size for path in mount_root.iterdir() if path.is_file())
                demand(summary['task_mount_retained_file_bytes']==0,'unexpected files in owned mount backing directory')
                mount_root.rmdir()
            except BaseException as error:cleanup_errors.append('unmount:'+repr(error))
        remaining=[item for item in owned(os.getpid(),session) if item['starttime']>=root_identity['starttime']]
        for sig,handler in saved_handlers.items():cleanup_step(cleanup_errors,'handler',lambda sig=sig,handler=handler:signal.signal(sig,handler))
        summary['cleanup']=dict(unknown=list(identity_cleanup.unknown.values()),forced=forced,errors=cleanup_errors,remaining=remaining,instance_removed=removed,mount_restored=unmounted,driver_reaped=driver is None or driver.returncode is not None)
        if identity_cleanup.unknown or cleanup_errors or forced or remaining or not removed or not unmounted:summary['status']='invalid'
        summary['wall_seconds']=time.monotonic()-started
        if not cleanup_evidence(output/'marker-launcher.json',summary,write_json,cleanup_errors):summary['status']='invalid'
    return 0 if summary['status']=='valid' else 1


def supervise(args):
    """Independent parent clock bounds the worker even when its filesystem walk stalls."""
    output=Path(args.output)
    role=Path(__file__).resolve().parents[2]/'.cache/v0.5.1-s4'/args.role
    demand(output.is_absolute() and output.parent==role and re.fullmatch(r'run-[A-Za-z0-9_-]+',output.name)
           and output.is_dir() and not any(path.is_symlink() for path in (output,*output.parents)),
           'supervisor requires admitted exact run root')
    ledger=json.loads((role/'ledger.json').read_text())
    records=[item for item in ledger['runs'] if item['run_id']==output.name]
    demand(len(records)==1 and records[0]['status']=='running' and records[0]['kind'] in ('observed_abba','confirmation_ab')
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
        except BaseException as error:
            try:publish_worker_failure(args,error)
            except BaseException as evidence_error:
                try:emit_error(dict(first_error=error_record(error,'marker.worker.startup',os.getpid()),evidence_error=error_record(evidence_error,'marker.worker.evidence',os.getpid())))
                except BaseException:pass
            os._exit(1)
    record=None;namespace_fd=None;forced=False;errors=[];reaped=[];status=None;session=None;first_error=None;worker_fault=None;verified_not_created=False
    try:
        record=identity(child)
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
                        if item['starttime']>=supervisor_identity['starttime']]+([record] if record is not None else [])
            for item in candidates:
                if alive(item):cleanup_step(errors,'kill',lambda item=item:os.kill(item['pid'],signal.SIGKILL))
            if record is not None and alive(record):cleanup_step(errors,'kill_worker',lambda:os.kill(child,signal.SIGKILL))
            if namespace_fd is not None:
                demand(libc.setns(namespace_fd,0x20000)==0,'supervisor enter owned mount namespace failed')
                plan=json.loads((output/'marker-resource-plan.json').read_text())
                demand(plan['root_identity']['pid']==child and plan['root_identity']['starttime']==record['starttime'],'resource owner mismatch')
                instance=Path(plan['instance']);mount_root=Path(plan['mount_directory'])
                if instance.exists():
                    cleanup_step(errors,'trace_off',lambda:(instance/'tracing_on').write_text('0'))
                    cleanup_step(errors,'events_off',lambda:(instance/'events/enable').write_text('0'))
                    cleanup_step(errors,'instance_remove',instance.rmdir)
                if mount_present(mount_root):subprocess.run(['/usr/bin/umount',str(mount_root)],check=True,timeout=max(0,deadline-time.monotonic()-.2))
                if mount_root.exists():mount_root.rmdir()
    except BaseException as error:
        first_error=error_record(error,'inner.supervisor.work',child if record is None else None)
        errors.append(first_error)
    finally:
        identity_cleanup.cleaning=True
        if record is None:identity_cleanup.unknown[child]=dict(identity=dict(pid=child),error=first_error)
        fault_path=output/'marker-worker-failure.json'
        try:
            fault_exists=fault_path.exists()
        except BaseException as fault_error:
            fault_exists=False;errors.append(error_record(fault_error,'inner.supervisor.worker_failure_presence',child))
        if fault_exists:
            try:
                worker_fault=json.loads(fault_path.read_text())
                owner=worker_fault['worker_identity']
                demand(record is not None and owner['pid']==child and owner['starttime']==record['starttime'] and worker_fault['run_id']==output.name,'worker failure owner/run mismatch')
                if first_error is None:first_error=worker_fault['first_error']
                errors.append(worker_fault['first_error'])
                verified_not_created=worker_fault['resource_state']=='not_created' and worker_fault['worker_phase'] in ('admission','tool-verification','runtime-verification','preparation') and not (output/'marker-resource-plan.json').exists()
            except BaseException as fault_error:errors.append(error_record(fault_error,'inner.supervisor.worker_failure',child))
        # Every exit path owns cleanup, including an early worker failure or metadata error.
        try:
            driver_path=output/'marker-driver-identity.json'
            if driver_path.exists():session=json.loads(driver_path.read_text())['session']
            candidates=[item for item in owned(os.getpid(),session)
                        if item['starttime']>=supervisor_identity['starttime']]+([record] if record is not None else [])
            for item in candidates:
                if alive(item):
                    forced=True;cleanup_step(errors,'kill',lambda item=item:os.kill(item['pid'],signal.SIGKILL))
            if record is not None and alive(record):forced=True;cleanup_step(errors,'kill_worker',lambda:os.kill(child,signal.SIGKILL))
        except BaseException as error:errors.append('final owned cleanup:'+repr(error))
        restored=False
        try:
            if namespace_fd is not None:
                demand(libc.setns(namespace_fd,0x20000)==0,'final owned namespace enter failed')
                plan=json.loads((output/'marker-resource-plan.json').read_text())
                demand(plan['root_identity']['pid']==child and plan['root_identity']['starttime']==record['starttime'],'final resource owner mismatch')
                instance=Path(plan['instance']);mount_root=Path(plan['mount_directory'])
                if instance.exists():
                    cleanup_step(errors,'trace_off',lambda:(instance/'tracing_on').write_text('0'))
                    cleanup_step(errors,'events_off',lambda:(instance/'events/enable').write_text('0'))
                    cleanup_step(errors,'instance_remove',instance.rmdir)
                if mount_present(mount_root):subprocess.run(['/usr/bin/umount',str(mount_root)],check=True,timeout=max(0,deadline-time.monotonic()-.2))
                if mount_root.exists():mount_root.rmdir()
                restored=not instance.exists() and not mount_present(mount_root) and not mount_root.exists()
            elif verified_not_created:
                restored=True
            else:
                worker_summary=json.loads((output/'marker-launcher.json').read_text())
                restored=worker_summary['cleanup']['instance_removed'] and worker_summary['cleanup']['mount_restored']
        except BaseException as error:errors.append('final resource cleanup:'+repr(error))
        if namespace_fd is not None:cleanup_step(errors,'namespace_fd',lambda:os.close(namespace_fd))
        while time.monotonic()<deadline-.1:
            try:
                pid,code=os.waitpid(-1,os.WNOHANG)
                if pid:reaped.append(pid);continue
                adopted=[item for item in owned(os.getpid(),session)
                         if item['starttime']>=supervisor_identity['starttime']]
                for item in adopted:
                    if alive(item):forced=True;cleanup_step(errors,'kill',lambda item=item:os.kill(item['pid'],signal.SIGKILL))
                if not adopted and (record is None or not alive(record)):break
            except ChildProcessError:break
            except BaseException as error:errors.append(error_record(error,'inner.cleanup.adopted_wait'));break
            time.sleep(.005)
        remaining=[item for item in owned(os.getpid(),session)
                   if item['starttime']>=supervisor_identity['starttime']]
        try:
            if verified_not_created:raise RuntimeError('worker failed before private resources existed; first cause preserved')
            worker_summary=json.loads((output/'marker-launcher.json').read_text())
            demand(worker_summary['status']=='valid' and not worker_summary['cleanup']['errors']
                   and not worker_summary['cleanup']['forced'] and not worker_summary['cleanup']['remaining'],
                   'worker cleanup/status invalid')
        except BaseException as error:errors.append('worker final validation:'+repr(error))
        if identity_cleanup.unknown:errors.append('owned identity unknown')
        if remaining or not restored:errors.append('owned processes/resources remain or restoration unknown')
        for sig,handler in saved.items():cleanup_step(errors,'handler',lambda sig=sig,handler=handler:signal.signal(sig,handler))
        cleanup_evidence(output/'marker-supervisor.json',dict(first_error=first_error,unknown=list(identity_cleanup.unknown.values()),forced=forced,errors=errors,reaped=reaped,
                   remaining=remaining,resources_restored=restored,resource_state='not_created_verified' if verified_not_created else 'restored_verified' if restored else 'unknown',worker_failure=worker_fault,
                   interrupted=interrupted,worker_identity=record,worker_wait_status=status,wall_seconds=time.monotonic()-args.started),write_json,errors)
    return 1 if forced or errors or status!=0 else 0


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--role',choices=('builder','reviewer'),default='builder')
    parser.add_argument('--output',required=True);parser.add_argument('--manifest',required=True)
    parser.add_argument('--seconds',type=int,required=True);parser.add_argument('--requests-per-connection',type=int,default=0)
    parser.add_argument('--connections',type=int,required=True)
    parser.add_argument('--warmup',type=int,required=True);parser.add_argument('--duration',type=int,required=True);parser.add_argument('--detailed',action='store_true')
    raise SystemExit(supervise(parser.parse_args()))
