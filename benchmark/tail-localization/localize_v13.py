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
from budget_v11 import BudgetError, Reservation, atomic_json
from r015_admission import prepare_environment, record_build_artifact
from protection import verify_protected


from proc_identity_v11 import process_identity,same_process,owned_descendants,error_record
from cleanup_v11 import cleanup_owned,emit_error


def execute(args,repository=None):
    repository = Path(repository) if repository is not None else Path(__file__).resolve().parents[2]
    stage = repository / ".cache/v0.5.1-s4"
    if args.seconds <= 7:
        raise BudgetError("total deadline must reserve 7 seconds for cleanup")
    with Reservation(stage, args.role, args.run_id, args.kind, args.seconds, args.static_inventory_sha256, args.static_inventory_name) as reservation:
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



def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--role", choices=("builder", "reviewer"), required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--kind", required=True)
    parser.add_argument("--seconds", type=float, required=True)
    parser.add_argument("--static-inventory-sha256")
    parser.add_argument("--static-inventory-name", default="static-inventory-v6.json")
    parser.add_argument("--r015-package-sha256")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.command[:1] == ["--"]:
        args.command.pop(0)
    if not args.command:
        parser.error("command is required")
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
