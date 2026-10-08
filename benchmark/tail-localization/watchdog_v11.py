"""R009 independent deadline guard: unknown identity is never a signal target."""
import json
import os
from pathlib import Path
import select
import signal
import sys
import time
from proc_identity_v11 import process_identity as identity,proc_candidates,error_record


def supervise(parent,starttime,soft_deadline,hard_deadline,pipe_fd,output):
    owner=dict(pid=parent,starttime=starttime);owned={};errors=[];unknown={};terminated=False;abnormal=False;forced=[]
    def status(record):
        try:
            current=identity(record['pid'])
            if current is None or current['starttime']!=record['starttime'] or current['state']=='Z':return 'exited'
            return 'live'
        except BaseException as error:
            unknown[record['pid']]=dict(identity=record,error=error_record(error,'watchdog.identity',record['pid']))
            return 'unknown'
    def failure(error,phase):
        errors.append(error_record(error,phase))
    def send(record,sig):
        if status(record)!='live':return
        try:os.kill(record['pid'],sig)
        except ProcessLookupError:pass
        except BaseException as error:errors.append(error_record(error,'watchdog.signal',record['pid']))
    def finish(reason):
        remaining=[record for record in owned.values() if status(record)=='live']
        result=dict(forced=bool(forced),parent_abnormal=abnormal,identities=list(owned.values()),remaining=remaining,unknown=list(unknown.values()),errors=errors,reason=reason,deadline=hard_deadline)
        try:Path(output).write_text(json.dumps(result)+'\n')
        except BaseException as error:
            failure(error,'watchdog.evidence');print(json.dumps(dict(evidence_written=False,watchdog=result)),file=sys.stderr,flush=True)
        return 1 if abnormal or errors or unknown or remaining or forced else 0
    while time.monotonic()<hard_deadline:
        try:
            ready,_,_=select.select([pipe_fd],[],[],min(.02,max(0,hard_deadline-time.monotonic())))
            if ready:
                message=os.read(pipe_fd,1)
                if message==b'D':
                    if errors or unknown or any(status(record)!='exited' for record in owned.values()):
                        abnormal=True;errors.append(dict(type='UnsafeCompletion',errno=None,phase='watchdog.notification',pid=parent,message='D before verified cleanup'));return finish('unsafe completion')
                    return 0
                if not message:abnormal=True
            candidates=[]
            for pid in proc_candidates():
                if pid==os.getpid():continue
                record=identity(pid)
                if record is not None:candidates.append(record)
            parents=({parent} if status(owner)=='live' else set())|{pid for pid,record in owned.items() if status(record)=='live'}
            command_path=Path(output).with_name('process.json')
            command=json.loads(command_path.read_text()) if command_path.exists() else None
            if command and command.get('session')==command['pid']:
                for record in candidates:
                    if record['session']==command['session'] and record['starttime']>=command['starttime']:
                        owned[record['pid']]=record;parents.add(record['pid'])
            changed=True
            while changed:
                changed=False
                for record in candidates:
                    if record['ppid'] in parents and record['pid'] not in parents:
                        owned[record['pid']]=record;parents.add(record['pid']);changed=True
        except BaseException as error:
            failure(error,'watchdog.scan');abnormal=True
            if getattr(error,'target_pid',None) is not None:unknown[error.target_pid]=dict(identity=dict(pid=error.target_pid),error=error_record(error,'watchdog.scan'))
        if status(owner)!='live':abnormal=True
        if (time.monotonic()>=soft_deadline or abnormal) and not terminated:
            send(owner,signal.SIGTERM)
            for record in owned.values():send(record,signal.SIGTERM)
            terminated=True
        if abnormal and not any(status(record)=='live' for record in owned.values()) and not unknown:return finish('abnormal owner/pipe')
    for record in owned.values():
        if status(record)=='live':send(record,signal.SIGKILL);forced.append(record)
    if status(owner)=='live':send(owner,signal.SIGKILL)
    return finish('absolute deadline') or 1


if __name__=='__main__':
    raise SystemExit(supervise(int(sys.argv[1]),int(sys.argv[2]),float(sys.argv[3]),float(sys.argv[4]),int(sys.argv[5]),sys.argv[6]))
