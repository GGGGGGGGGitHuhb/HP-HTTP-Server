"""Owned-session supervision for check/unit descendants, including interruptions."""
import ctypes
import os
import pathlib
import resource
import signal
import subprocess
import time
from identity import legacy


def resources(root):
    result = legacy.resources_ok(root)
    limit = result['nofile']
    legacy.demand(limit == resource.RLIM_INFINITY or limit >= 1024, 'nofile below 1024')
    return result


def processes():
    result = {}
    for directory in pathlib.Path('/proc').iterdir():
        if not directory.name.isdigit():
            continue
        try:
            fields = (directory/'stat').read_text().rsplit(')',1)[1].split()
            result[int(directory.name)] = {'starttime':int(fields[19]), 'state':fields[0], 'parent':int(fields[1]), 'session':int(fields[3])}
        except (FileNotFoundError, ProcessLookupError):
            pass
    return result


class Scope:
    def __init__(self, child, previous_children):
        self.child = child
        self.previous_children = previous_children
        self.starttime = legacy.process_info(child.pid)['starttime']
        self.owned = {child.pid:self.starttime}

    def members(self):
        table = processes()
        current = {pid:info for pid,info in table.items() if self.owned.get(pid) == info['starttime']}
        # This wrapper is a subreaper with one command. Existing children were
        # recorded before Popen and cannot become part of this operation. Adopted
        # descendants remain owned even after setsid or double-fork.
        for pid, info in table.items():
            if info['parent'] == os.getpid() and pid not in self.previous_children and info['starttime'] >= self.starttime:
                current[pid] = info
        changed = True
        while changed:
            changed = False
            for pid,info in table.items():
                if pid not in current and info['parent'] in current:
                    current[pid] = info
                    changed = True
        self.owned.update({pid:info['starttime'] for pid,info in current.items()})
        return current

    def signal(self, sig):
        current = self.members()
        for pid, info in current.items():
            if info['state'] != 'Z' and processes().get(pid,{}).get('starttime') == info['starttime']:
                try:
                    os.kill(pid,sig)
                except ProcessLookupError:
                    pass
        return current

    def reap(self):
        self.child.poll()
        for pid in self.members():
            if pid == self.child.pid:
                continue
            try:
                os.waitpid(pid,os.WNOHANG)
            except ChildProcessError:
                pass

    def cleanup(self, grace=7):
        initial = self.signal(signal.SIGTERM)
        deadline = time.monotonic()+grace
        while self.members() and time.monotonic()<deadline:
            self.reap()
            time.sleep(.02)
        forced = bool(self.members())
        if forced:
            self.signal(signal.SIGKILL)
        deadline = time.monotonic()+3
        while self.members() and time.monotonic()<deadline:
            self.reap()
            time.sleep(.02)
        self.child.wait(timeout=3)
        self.reap()
        return {'pid':self.child.pid,'starttime':self.starttime,'returncode':self.child.returncode,'forced':forced,'reaped':not self.members(),'owned_before_cleanup':initial,'owned_identities':self.owned}


def run(command, root, prefix, timeout=170):
    """No shell. Track descendants across session changes using parentage and subreaping.

    Child subreaping is process-local and restores its previous setting. We signal
    each observed PID only after checking its session and /proc starttime.
    """
    resources(root)
    legacy.log_guard(root)
    libc=ctypes.CDLL(None,use_errno=True)
    previous=ctypes.c_int()
    if libc.prctl(37,ctypes.byref(previous),0,0,0) != 0: # PR_GET_CHILD_SUBREAPER
        raise OSError(ctypes.get_errno(),'PR_GET_CHILD_SUBREAPER')
    if libc.prctl(36,1,0,0,0) != 0: # PR_SET_CHILD_SUBREAPER
        raise OSError(ctypes.get_errno(),'PR_SET_CHILD_SUBREAPER')
    child=None
    scope=None
    outcome={}
    previous_children={pid for pid,info in processes().items() if info['parent']==os.getpid()}
    try:
        with pathlib.Path(str(prefix)+'.stdout').open('wb') as stdout, pathlib.Path(str(prefix)+'.stderr').open('wb') as stderr:
            child=subprocess.Popen(command,stdout=stdout,stderr=stderr,start_new_session=True)
            # Session leader remains unreaped until poll; /proc exposes identity even
            # for immediate exits, so creation failures can still be safely collected.
            scope=Scope(child,previous_children)
            starttime=scope.starttime
            legacy.save(str(prefix)+'.process.json',{'command':command,'pid':child.pid,'starttime':starttime,'session':child.pid})
            began=time.monotonic()
            try:
                while child.poll() is None:
                    scope.members()
                    legacy.log_guard(root)
                    legacy.demand(time.monotonic()-began <= timeout,'check/unit watchdog')
                    time.sleep(.05)
                legacy.log_guard(root)
                legacy.demand(child.returncode==0,'check/unit nonzero exit')
                legacy.demand(not scope.members(),'check/unit left owned descendants')
            finally:
                outcome=scope.cleanup()
                legacy.save(str(prefix)+'.cleanup.json',outcome)
            legacy.demand(not outcome['forced'] and outcome['reaped'],'check/unit forced or unreaped descendants')
            return outcome
    finally:
        # A failure between Popen and obtaining /proc identity still owns this Popen.
        if child is not None and not outcome:
            scope = scope or Scope(child,previous_children)
            outcome=scope.cleanup()
            legacy.save(str(prefix)+'.cleanup.json',outcome)
        libc.prctl(36,previous.value,0,0,0)
