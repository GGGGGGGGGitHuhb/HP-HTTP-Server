"""Original executor load/statistics retained; only ownership/close adapter changes."""
import errno
from pathlib import Path
import subprocess
import time
from cleanup_v11 import emit_error
from proc_identity_v11 import error_record


def matches_owner(reader,owner):
    try:return reader(owner['pid'])['starttime']==owner['starttime']
    except OSError as error:
        if error.errno in (errno.ENOENT,errno.ESRCH):return False
        error.target_pid=owner['pid'];raise


def adapt_executor(executor,work_deadline):
    original=executor.OwnedProcess
    class MissingAwareOwnedProcess(original):
        def matches(self):return matches_owner(executor.process_info,self.identity)

        def close(self,timeout=7):
            errors=[];first=None;unknown=[]
            deadline=min(work_deadline,time.monotonic()+min(timeout,7))
            def step(label,action):
                nonlocal first
                try:return action()
                except BaseException as error:
                    if first is None:first=error
                    errors.append(error_record(error,'staircase.close.'+label,self.identity['pid']))
                    if label.startswith('identity'):unknown.append(dict(pid=self.identity['pid'],starttime=self.identity['starttime']))
            if step('alive',self.alive) is True and step('identity.term',self.matches) is True:step('term',self.process.terminate)
            # Direct-child wait never requires a PID signal and is attempted even after identity failure.
            step('wait',lambda:self.process.wait(timeout=max(0,deadline-time.monotonic())))
            if self.process.returncode is None and step('identity.kill',self.matches) is True:
                self.forced=True;step('kill',self.process.kill)
                step('reap',lambda:self.process.wait(timeout=max(0,min(.2,deadline-time.monotonic()))))
            step('stdout',self.stdout.close);step('stderr',self.stderr.close)
            self.stop_result=self.process.returncode
            result=dict(pid=self.identity['pid'],starttime=self.identity['starttime'],returncode=self.stop_result,forced=self.forced,reaped=self.process.returncode is not None,remaining=[] if self.process.returncode is not None else [self.identity],unknown=unknown,errors=errors)
            result['first_error']=errors[0] if errors else None
            step('evidence',lambda:executor.save(str(Path(str(self.stdout_path).removesuffix('.stdout')))+'.cleanup.json',result))
            result['first_error']=errors[0] if errors else None
            if errors:
                step('stderr_fallback',lambda:emit_error(dict(staircase_cleanup=result)))
                raise RuntimeError('original staircase owner close failed') from first
            return result
    executor.OwnedProcess=MissingAwareOwnedProcess
