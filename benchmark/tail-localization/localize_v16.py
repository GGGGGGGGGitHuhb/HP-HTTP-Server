"""S4 entry point: every execution is reserved before invoking its command."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
# Only standard-library code runs before the durable one-use invocation claim.
R019_CLAIM = None

def claim_before_candidate_import(argv):
    import fcntl
    import stat
    def value(flag):
        if argv.count(flag) != 1:
            raise ValueError('R019 exact invocation flag')
        return argv[argv.index(flag) + 1]
    def read_control(path):
        path=Path(path)
        if not path.is_absolute() or '..' in path.parts:
            raise ValueError('R019 control absolute path')
        descriptors=[]
        try:
            parent=os.open('/',os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW);descriptors.append(parent)
            for part in path.parts[1:-1]:
                before_parent=os.stat(part,dir_fd=parent,follow_symlinks=False)
                child=os.open(part,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW,dir_fd=parent);descriptors.append(child)
                opened_parent=os.fstat(child)
                if (before_parent.st_dev,before_parent.st_ino,before_parent.st_mode)!=(opened_parent.st_dev,opened_parent.st_ino,opened_parent.st_mode):
                    raise ValueError('R019 control parent exchanged')
                parent=child
            before=os.stat(path.name,dir_fd=parent,follow_symlinks=False)
            if not stat.S_ISREG(before.st_mode):raise ValueError('R019 control regular required')
            descriptor=os.open(path.name,os.O_RDONLY|os.O_NONBLOCK|os.O_NOFOLLOW,dir_fd=parent);descriptors.append(descriptor)
            opened=os.fstat(descriptor)
            if not stat.S_ISREG(opened.st_mode) or (before.st_dev,before.st_ino,before.st_size)!=(opened.st_dev,opened.st_ino,opened.st_size) or opened.st_size>2*1024**2:
                raise ValueError('R019 control file identity/size')
            parts=[]
            while True:
                block=os.read(descriptor,65536)
                if not block:break
                parts.append(block)
                if sum(map(len,parts))>2*1024**2:raise ValueError('R019 control grew')
            after=os.fstat(descriptor)
            if (opened.st_dev,opened.st_ino,opened.st_size)!=(after.st_dev,after.st_ino,after.st_size):
                raise ValueError('R019 control changed')
            return b''.join(parts)
        finally:
            primary=sys.exc_info()[1];errors=[]
            for descriptor in reversed(descriptors):
                try:os.close(descriptor)
                except OSError as error:errors.append(error)
            if errors:
                if primary is not None:primary.add_note('R019 control close: '+repr(errors))
                else:raise errors[0]
    role, run = value('--role'), value('--run-id')
    required = run == 'run-r019-check-001' or (role == 'builder' and run == 'run-r018-build-001')
    if not required:
        return None
    if role not in ('builder', 'reviewer'):
        raise ValueError('R019 role')
    stage = Path(__file__).absolute().parents[2] / '.cache/v0.5.1-s4'
    suffix = 'check-used-001' if run == 'run-r019-check-001' else 'build-used-002'
    used = stage / role / 'cache' / ('r019-' + suffix + '.json')
    claim_fds=[]
    try:
        parent=os.open('/',os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW);claim_fds.append(parent)
        stage_fd=None
        for part in used.parent.parts[1:]:
            before=os.stat(part,dir_fd=parent,follow_symlinks=False)
            child=os.open(part,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW,dir_fd=parent);claim_fds.append(child)
            opened=os.fstat(child)
            if (before.st_dev,before.st_ino,before.st_mode)!=(opened.st_dev,opened.st_ino,opened.st_mode):
                raise ValueError('R019 claim parent exchanged')
            parent=child
            if part=='v0.5.1-s4':stage_fd=child
        if stage_fd is None:raise ValueError('R019 claim stage')
        lock_fd=os.open('r019-invocation.lock',os.O_RDWR|os.O_CREAT|os.O_NONBLOCK|os.O_NOFOLLOW,0o600,dir_fd=stage_fd);claim_fds.append(lock_fd)
        if not stat.S_ISREG(os.fstat(lock_fd).st_mode):raise ValueError('R019 lock regular required')
        fcntl.flock(lock_fd,fcntl.LOCK_EX)
        fd=os.open(used.name,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600,dir_fd=parent)
        with os.fdopen(fd,'w') as stream:
            json.dump({'schema':'r019-invocation-used-v1','role':role,'run_id':run,'invocation_pid':os.getpid(),'actual_argv':argv},stream)
            stream.flush();os.fsync(stream.fileno())
        os.fsync(parent)
    finally:
        primary=sys.exc_info()[1];errors=[]
        for descriptor in reversed(claim_fds):
            try:os.close(descriptor)
            except OSError as error:errors.append(error)
        if errors:
            if primary is not None:primary.add_note('R019 claim close: '+repr(errors))
            else:raise errors[0]
    called = stage / 'leader' / ('r019-' + role + ('-check-called-001.json' if run == 'run-r019-check-001' else '-build-called-002.json'))
    expected = value('--r019-called-sha256')
    raw = read_control(called)
    if hashlib.sha256(raw).hexdigest() != expected:
        raise ValueError('R019 called hash')
    record = json.loads(raw)
    table = Path(record['command_table_path'])
    table_raw = read_control(table)
    if hashlib.sha256(table_raw).hexdigest() != record['command_table_sha256']:
        raise ValueError('R019 command table drift')
    rows = [row for row in json.loads(table_raw)['commands'] if row['run_id'] == run]
    if len(rows) != 1:
        raise ValueError('R019 command row')
    row = rows[0]
    canonical = json.dumps(row,sort_keys=True,separators=(',',':')).encode()
    normalized = list(argv); normalized[normalized.index('--r019-called-sha256')+1] = 'R019_CALLED_SHA256'
    if hashlib.sha256(canonical).hexdigest() != record['command_row_sha256'] or row['argv'] != normalized:
        raise ValueError('R019 exact command binding')
    if record['role'] != role or record['run_id'] != run or any(os.environ.get(k) != v for k,v in row['environment'].items()):
        raise ValueError('R019 called identity/environment')
    seal = Path(record['execution_seal_path'])
    if hashlib.sha256(read_control(seal)).hexdigest() != record['execution_seal_sha256']:
        raise ValueError('R019 execution seal drift')
    return {'path':str(used),'pid':os.getpid(),'called':record,'called_sha256':expected}

if __name__ == '__main__':
    R019_CLAIM = claim_before_candidate_import(['/usr/bin/python3', str(Path(__file__).absolute()), *sys.argv[1:]])

from budget_v14 import BudgetError, Reservation, atomic_json
from r016.r015_admission import prepare_environment, record_build_artifact, recovery_context
from protection import verify_protected
from r019_admission import prepare_environment as prepare_r018_environment, record_build_artifact as record_r018_artifact


from proc_identity_v11 import process_identity,same_process,owned_descendants,error_record
from cleanup_v11 import cleanup_owned,emit_error
import r019_admission
r019_admission.CURRENT_CLAIM = R019_CLAIM


def execute(args,repository=None):
    repository = Path(repository) if repository is not None else Path(__file__).resolve().parents[2]
    stage = repository / ".cache/v0.5.1-s4"
    if args.seconds <= 7:
        raise BudgetError("total deadline must reserve 7 seconds for cleanup")
    context = lambda: recovery_context(args, repository, ["/usr/bin/python3", str(Path(__file__).absolute()), *sys.argv[1:]])
    with Reservation(stage, args.role, args.run_id, args.kind, args.seconds, args.static_inventory_sha256, args.static_inventory_name, context) as reservation:
        process = None
        root = None
        identities = []
        forced = False
        errors = []
        supervisor = None
        watchdog_read = None
        watchdog_write = None
        self_identity = None
        first_error = None
        previous_term=None
        try:
            previous_term=signal.getsignal(signal.SIGTERM)
            libc = ctypes.CDLL(None, use_errno=True)
            if libc.prctl(36, 1, 0, 0, 0) != 0:  # PR_SET_CHILD_SUBREAPER
                raise BudgetError("subreaper unavailable")
            def deadline_signal(signum, frame):
                raise BudgetError("watchdog requested bounded cleanup")
            signal.signal(signal.SIGTERM, deadline_signal)
            environment = os.environ.copy()
            role_root = stage / args.role
            for name in ("tmp", "cache"):
                (role_root / name).mkdir(exist_ok=True)
            environment.update(TMPDIR=str(role_root / "tmp"), TMP=str(role_root / "tmp"),
                               TEMP=str(role_root / "tmp"), XDG_CACHE_HOME=str(role_root / "cache"),
                               PYTHONDONTWRITEBYTECODE="1")
            self_identity = process_identity(os.getpid())
            prepare_environment(args, reservation, repository, environment, self_identity)
            prepare_r018_environment(args, reservation, repository, environment, self_identity)
            atomic_json(reservation.output / "command.json", dict(argv=args.command, cwd=str(repository),
                        deadline_seconds=args.seconds, cleanup_reserved_seconds=7,
                        environment={key: value for key, value in environment.items()
                                     if key in ("TMPDIR", "TMP", "TEMP", "XDG_CACHE_HOME", "PYTHONDONTWRITEBYTECODE", "LD_LIBRARY_PATH", "LUA_PATH", "LUA_CPATH")
                                     or key.startswith("HP_BASELINE_")}))

            watchdog_read, watchdog_write = os.pipe()
            self_identity = process_identity(os.getpid())
            with (reservation.output / "watchdog.stdout").open("xb") as watchdog_stdout, (reservation.output / "watchdog.stderr").open("xb") as watchdog_stderr:
                supervisor = subprocess.Popen([sys.executable, str(Path(__file__).with_name("watchdog_v11.py")),
                        str(os.getpid()), str(self_identity["starttime"]), str(reservation.started + args.seconds - 7),
                        str(reservation.started + args.seconds), str(watchdog_read), str(reservation.output / "watchdog-forced.json")],
                        pass_fds=(watchdog_read,), env=environment, stdout=watchdog_stdout, stderr=watchdog_stderr)
            os.close(watchdog_read)
            watchdog_read=None
            protection_before = verify_protected(repository)
            atomic_json(reservation.output / "protection-before.json", protection_before)
            if not protection_before["match"]:
                raise BudgetError("protected files differ before execution")
            with (reservation.output / "command.stdout").open("xb") as stdout, (reservation.output / "command.stderr").open("xb") as stderr:
                process = subprocess.Popen(args.command, cwd=repository, env=environment, stdout=stdout,
                                           stderr=stderr, start_new_session=True)
                root = process_identity(process.pid)
                if root is None:
                    raise BudgetError("child identity disappeared before acquisition")
                atomic_json(reservation.output / "process.json", root)
                work_deadline = reservation.started + args.seconds - 7
                while process.poll() is None:
                    identities = owned_descendants(root, identities)
                    # A child exiting between discovery polls is adopted by this
                    # process-local subreaper. Acquire its identity before reaping.
                    for path in Path("/proc").iterdir():
                        if path.name.isdigit() and int(path.name) != supervisor.pid:
                            adopted = process_identity(int(path.name))
                            if adopted and adopted["ppid"] == os.getpid() and adopted["pid"] != process.pid and adopted["starttime"] >= self_identity["starttime"]:
                                if not any(item["pid"] == adopted["pid"] and item["starttime"] == adopted["starttime"] for item in identities):
                                    identities.append(adopted)
                    reservation.guard()
                    if time.monotonic() >= work_deadline:
                        raise BudgetError("work deadline reached; cleanup starts")
                    time.sleep(0.05)
                if process.returncode:
                    raise BudgetError(f"command failed: {process.returncode}")
                record_build_artifact(args, reservation, environment)
                record_r018_artifact(args, reservation, environment)
        except BaseException as error:
            first_error=error_record(error,'work',getattr(error,'target_pid',None) or (process.pid if process is not None else None))
            raise
        finally:
            cleanup_deadline=min(time.monotonic()+7,reservation.started+args.seconds)
            cleanup_signals=[]
            preparation_errors=[]
            try:signal.signal(signal.SIGTERM,lambda signum,frame:cleanup_signals.append(signum))
            except BaseException as error:preparation_errors.append(error_record(error,'cleanup.handler'))
            if watchdog_read is not None:
                try:os.close(watchdog_read)
                except BaseException as error:preparation_errors.append(error_record(error,'cleanup.pipe_read_close'))
            try:
                result=cleanup_owned(process,root,identities,supervisor,watchdog_write,self_identity,
                    cleanup_deadline,atomic_json,lambda:verify_protected(repository),reservation.output,first_error,preparation_errors)
                if cleanup_signals:
                    result['complete']=False
                    result['errors'].append(dict(type='CleanupSignal',phase='cleanup.signal',errno=None,pid=os.getpid(),message=str(cleanup_signals)))
                    atomic_json(reservation.output/'cleanup.json',result)
                if not result['complete']:
                    if first_error is None:raise BudgetError('owned cleanup incomplete/unknown; see cleanup evidence')
                    emit_error(dict(first_error=first_error,cleanup=result))
            except BaseException as cleanup_error:
                if first_error is None:raise
                emit_error(dict(first_error=first_error,cleanup_error=error_record(cleanup_error,'cleanup.unhandled'),complete=False))
            finally:
                if previous_term is not None:
                    try:signal.signal(signal.SIGTERM,previous_term)
                    except BaseException as restore_error:
                        detail=error_record(restore_error,'cleanup.restore_handler')
                        emit_error(dict(first_error=first_error,cleanup_error=detail,complete=False))
                        try:
                            result['complete']=False;result['errors'].append(detail)
                            atomic_json(reservation.output/'cleanup.json',result)
                        except BaseException as evidence_error:emit_error(dict(error=error_record(evidence_error,'cleanup.restore_evidence'),evidence_written=False))
                        if first_error is None:raise



def parse_arguments(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--role", choices=("builder", "reviewer"), required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--kind", required=True)
    parser.add_argument("--seconds", type=float, required=True)
    parser.add_argument("--static-inventory-sha256")
    parser.add_argument("--static-inventory-name", default="static-inventory-v6.json")
    parser.add_argument("--r015-package-sha256")
    parser.add_argument("--r016-attempt-sha256")
    parser.add_argument("--r019-called-sha256")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    if args.command[:1] == ["--"]:
        args.command.pop(0)
    if not args.command:
        parser.error("command is required")
    return args

def main():
    args = parse_arguments()
    try:execute(args)
    except BaseException as error:
        settlement='unknown'
        try:
            ledger=json.loads((Path(__file__).resolve().parents[2]/'.cache/v0.5.1-s4'/args.role/'ledger.json').read_text())
            matching=[item for item in ledger['runs'] if item['run_id']==args.run_id]
            if len(matching)==1:settlement=matching[0]['status']
        except BaseException as settlement_error:
            emit_error(dict(error=error_record(settlement_error,'budget.settlement.read'),budget_settlement='unknown'))
        emit_error(dict(error=error_record(error,'wrapper.exit'),budget_settlement=settlement))
        raise


if __name__ == "__main__":
    main()
