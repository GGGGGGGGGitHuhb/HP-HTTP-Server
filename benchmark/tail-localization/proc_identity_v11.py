"""R009 process reads: absence is ENOENT/ESRCH; all other failures stay errors."""
import errno
from pathlib import Path


def error_record(error, phase, pid=None):
    return dict(type=type(error).__name__,errno=getattr(error,'errno',None),phase=phase,pid=pid if pid is not None else getattr(error,'target_pid',None),message=str(error))


def process_identity(pid):
    try:
        value=Path(f'/proc/{pid}/stat').read_text()
        fields=value[value.rfind(')')+2:].split()
        return dict(pid=pid,starttime=int(fields[19]),ppid=int(fields[1]),state=fields[0],pgrp=int(fields[2]),session=int(fields[3]))
    except OSError as error:
        if error.errno in (errno.ENOENT,errno.ESRCH):return None
        error.target_pid=pid
        raise
    except (ValueError,IndexError) as error:
        error.target_pid=pid
        raise


def same_process(record):
    current=process_identity(record['pid'])
    return current is not None and current['starttime']==record['starttime'] and current['state']!='Z'


def proc_candidates():
    return [int(path.name) for path in Path('/proc').iterdir() if path.name.isdigit()]


def owned_descendants(root,identities):
    found={record['pid']:record for record in identities if same_process(record)}
    if same_process(root):found[root['pid']]=root
    candidates=[]
    for pid in proc_candidates():
        record=process_identity(pid)
        if record is not None:candidates.append(record)
    changed=True
    while changed:
        changed=False
        for record in candidates:
            if record['ppid'] in found and record['pid'] not in found:
                found[record['pid']]=record;changed=True
    recorded={(record['pid'],record['starttime']):record for record in identities}
    recorded.update({(record['pid'],record['starttime']):record for record in found.values()})
    return list(recorded.values())
