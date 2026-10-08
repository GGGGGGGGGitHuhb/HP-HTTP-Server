"""Bounded R009 cleanup. A failed step cannot skip subsequent evidence/finalization."""
import json
import os
import signal
import subprocess
import sys
import time
from proc_identity_v11 import error_record,process_identity,owned_descendants,proc_candidates


def emit_error(value):
    encoded=json.dumps(value,sort_keys=True)
    try:print(encoded,file=sys.stderr,flush=True);return True
    except BaseException:
        try:os.write(2,(encoded+'\n').encode());return True
        except BaseException:return False


def cleanup_owned(process,root,identities,supervisor,pipe_fd,self_identity,deadline,write_evidence,verify_protection,output,first_error=None,initial_errors=()):
    result=dict(forced=False,errors=list(initial_errors),remaining=[],unknown=[],exited=[],adopted_reaped=[],direct_child_reaped=process is None,watchdog_notified=False,deadline=deadline,first_error=first_error)
    records={(item['pid'],item['starttime']):item for item in identities}
    if root is not None:records[(root['pid'],root['starttime'])]=root
    elif process is not None:result['unknown'].append(dict(identity=dict(pid=process.pid),error=dict(type='IdentityNotAcquired',phase='cleanup.identity',errno=None,pid=process.pid,message='child exists without verified starttime')))
    def attempt(phase,operation,pid=None):
        try:return operation()
        except BaseException as error:
            detail=error_record(error,phase,pid);result['errors'].append(detail)
            if phase=='cleanup.discovery':result['unknown'].append(dict(identity=dict(pid=detail['pid']),error=detail))
            return None
    def state(item):
        try:
            current=process_identity(item['pid'])
            if current is None or current['starttime']!=item['starttime'] or current['state']=='Z':return 'exited'
            return 'live'
        except BaseException as error:
            detail=dict(identity=item,error=error_record(error,'cleanup.identity',item['pid']))
            if not any(entry['identity'].get('pid')==item['pid'] for entry in result['unknown']):result['unknown'].append(detail)
            return 'unknown'
    def discover():
        if root is not None:
            for item in owned_descendants(root,list(records.values())):records[(item['pid'],item['starttime'])]=item
        for pid in proc_candidates():
            if supervisor is not None and pid==supervisor.pid:continue
            item=process_identity(pid)
            if item and self_identity and item['ppid']==self_identity['pid'] and item['starttime']>=self_identity['starttime']:
                records[(item['pid'],item['starttime'])]=item
    attempt('cleanup.discovery',discover)
    def send(item,sig):
        if state(item)!='live':return
        try:os.kill(item['pid'],sig)
        except ProcessLookupError:pass
    for item in reversed(list(records.values())):attempt('cleanup.term',lambda item=item:send(item,signal.SIGTERM),item['pid'])
    while time.monotonic()<deadline and any(state(item)=='live' for item in records.values()):
        time.sleep(min(.02,max(0,deadline-time.monotonic())))
    for item in records.values():
        if state(item)=='live':
            result['forced']=True
            attempt('cleanup.kill',lambda item=item:send(item,signal.SIGKILL),item['pid'])
    if process is not None:
        attempt('cleanup.direct_wait',lambda:process.wait(timeout=max(0,deadline-time.monotonic())),process.pid)
        result['direct_child_reaped']=process.returncode is not None
    for item in records.values():
        if process is not None and item['pid']==process.pid:continue
        def reap(item=item):
            try:
                pid,code=os.waitpid(item['pid'],os.WNOHANG)
                if pid:result['adopted_reaped'].append(dict(pid=pid,wait_status=code))
            except ChildProcessError:pass
        attempt('cleanup.adopted_wait',reap,item['pid'])
    for item in records.values():
        status=state(item)
        if status=='live':result['remaining'].append(item)
        elif status=='exited':result['exited'].append(item)
    result['identities']=list(records.values())
    confirmed=not result['remaining'] and not result['unknown'] and not result['errors'] and result['direct_child_reaped']
    if pipe_fd is not None:
        if confirmed:
            def notify():
                if os.write(pipe_fd,b'D')!=1:raise OSError('short watchdog completion notification')
                result['watchdog_notified']=True
            attempt('cleanup.watchdog_notify',notify)
        attempt('cleanup.watchdog_pipe_close',lambda:os.close(pipe_fd))
    if supervisor is not None:
        attempt('cleanup.watchdog_wait',lambda:supervisor.wait(timeout=max(0,deadline-time.monotonic())),supervisor.pid)
        if supervisor.returncode is None or supervisor.returncode!=0:
            result['errors'].append(dict(type='WatchdogFailure',errno=None,phase='cleanup.watchdog_status',pid=supervisor.pid,message='watchdog unknown/nonzero exit'))
    def protect():
        value=verify_protection();write_evidence(output/'protection-after.json',value)
        if not value['match']:raise RuntimeError('protected files differ after execution')
    attempt('cleanup.protection',protect)
    result['complete']=not result['forced'] and not result['errors'] and not result['remaining'] and not result['unknown'] and result['direct_child_reaped']
    try:write_evidence(output/'cleanup.json',result)
    except BaseException as error:
        result['complete']=False;result['errors'].append(error_record(error,'cleanup.evidence'))
        result['independent_output_available']=emit_error(dict(channel='stderr',cleanup=result,evidence_written=False))
    return result
