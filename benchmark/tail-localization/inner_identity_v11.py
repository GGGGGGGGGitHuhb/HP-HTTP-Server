"""Inner launcher cleanup classification; normal work reads remain fail closed."""
import os
from proc_identity_v11 import process_identity,proc_candidates,error_record

class CleanupIdentity:
    def __init__(self):self.cleaning=False;self.unknown={}
    def read(self,pid):
        try:return process_identity(pid)
        except BaseException as error:
            if not self.cleaning:raise
            self.unknown[pid]=dict(identity=dict(pid=pid),error=error_record(error,'inner.cleanup.identity',pid))
            return None
    def alive(self,record):
        current=self.read(record['pid'])
        return current is not None and current['starttime']==record['starttime'] and current['state']!='Z'
    def owned(self,root_pid,session):
        found=[]
        try:candidates=proc_candidates()
        except BaseException as error:
            if not self.cleaning:raise
            self.unknown[None]=dict(identity=dict(pid=None),error=error_record(error,'inner.cleanup.discovery'))
            return found
        for pid in candidates:
            item=self.read(pid)
            if item and item['pid']!=root_pid and (item['session']==session or item['ppid']==root_pid):found.append(item)
        return found


def cleanup_step(errors,label,operation):
    try:return operation()
    except BaseException as error:
        errors.append(error_record(error,'inner.cleanup.'+label));return None


def close_sample_resources(marker_fd,channel,streams,mapping,handlers,errors):
    import signal
    if marker_fd is not None:cleanup_step(errors,'marker_fd',lambda:os.close(marker_fd))
    if channel is not None:cleanup_step(errors,'channel',channel.close)
    for stream in streams:cleanup_step(errors,'stream',stream.close)
    cleanup_step(errors,'mapping',mapping.close)
    for sig,handler in handlers.items():cleanup_step(errors,'handler',lambda sig=sig,handler=handler:signal.signal(sig,handler))


def cleanup_evidence(path,value,writer,errors):
    from cleanup_v11 import emit_error
    try:writer(path,value);return True
    except BaseException as error:
        errors.append(error_record(error,'inner.cleanup.evidence'))
        emit_error(dict(evidence_written=False,path=str(path),value=value,errors=errors))
        return False


def required_identity(reader,pid,tid=None):
    """Registered/stable owners disappearing is a failed experiment, not a mapping."""
    try:return reader(pid,tid) if tid is not None else reader(pid)
    except BaseException as error:
        error.target_pid=pid
        raise
