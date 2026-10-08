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
from budget_v8 import BudgetError, Reservation, atomic_json
from protection import verify_protected


def process_identity(pid):
    try:
        value = Path(f"/proc/{pid}/stat").read_text()
    except FileNotFoundError:
        return None
    fields = value[value.rfind(")") + 2:].split()
    return dict(pid=pid, starttime=int(fields[19]), ppid=int(fields[1]), state=fields[0], pgrp=int(fields[2]), session=int(fields[3]))


def same_process(identity):
    current = process_identity(identity["pid"])
    return current is not None and current["starttime"] == identity["starttime"] and current["state"] != "Z"


def owned_descendants(root, identities):
    """Only acquire descendants while ancestry leads to an already owned identity."""
    found = {identity["pid"]: identity for identity in identities if same_process(identity)}
    if same_process(root):
        found[root["pid"]] = root
    candidates = []
    for path in Path("/proc").iterdir():
        if path.name.isdigit():
            current = process_identity(int(path.name))
            if current is not None:
                candidates.append(current)
    changed = True
    while changed:
        changed = False
        for current in candidates:
            if current["ppid"] in found and current["pid"] not in found:
                found[current["pid"]] = current
                changed = True
    recorded = {(identity["pid"], identity["starttime"]): identity for identity in identities}
    recorded.update({(identity["pid"], identity["starttime"]): identity for identity in found.values()})
    return list(recorded.values())


def execute(args):
    repository = Path(__file__).resolve().parents[2]
    stage = repository / ".cache/v0.5.1-s4"
    if args.seconds <= 7:
        raise BudgetError("total deadline must reserve 7 seconds for cleanup")
    with Reservation(stage, args.role, args.run_id, args.kind, args.seconds, args.static_inventory_sha256, args.static_inventory_name) as reservation:
        libc = ctypes.CDLL(None, use_errno=True)
        if libc.prctl(36, 1, 0, 0, 0) != 0:  # PR_SET_CHILD_SUBREAPER
            raise BudgetError("subreaper unavailable")
        previous_term = signal.getsignal(signal.SIGTERM)
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
        atomic_json(reservation.output / "command.json", dict(argv=args.command, cwd=str(repository),
                    deadline_seconds=args.seconds, cleanup_reserved_seconds=7,
                    environment={key: environment[key] for key in ("TMPDIR", "TMP", "TEMP", "XDG_CACHE_HOME", "PYTHONDONTWRITEBYTECODE")}))
        process = None
        root = None
        identities = []
        forced = False
        errors = []
        supervisor = None
        watchdog_write = None
        try:
            watchdog_read, watchdog_write = os.pipe()
            self_identity = process_identity(os.getpid())
            with (reservation.output / "watchdog.stdout").open("xb") as watchdog_stdout, (reservation.output / "watchdog.stderr").open("xb") as watchdog_stderr:
                supervisor = subprocess.Popen([sys.executable, str(Path(__file__).with_name("watchdog.py")),
                        str(os.getpid()), str(self_identity["starttime"]), str(reservation.started + args.seconds - 7),
                        str(reservation.started + args.seconds), str(watchdog_read), str(reservation.output / "watchdog-forced.json")],
                        pass_fds=(watchdog_read,), env=environment, stdout=watchdog_stdout, stderr=watchdog_stderr)
            os.close(watchdog_read)
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
        finally:
            if root is not None:
                identities = owned_descendants(root, identities)
                for path in Path("/proc").iterdir():
                    if path.name.isdigit() and (supervisor is None or int(path.name) != supervisor.pid):
                        adopted = process_identity(int(path.name))
                        if adopted and adopted["ppid"] == os.getpid() and adopted["pid"] != root["pid"] and adopted["starttime"] >= self_identity["starttime"]:
                            if not any(item["pid"] == adopted["pid"] and item["starttime"] == adopted["starttime"] for item in identities):
                                identities.append(adopted)
                for identity in reversed(identities):
                    if same_process(identity):
                        try:
                            os.kill(identity["pid"], signal.SIGTERM)
                        except ProcessLookupError:
                            pass
                cleanup_deadline = min(time.monotonic() + 7, reservation.started + args.seconds)
                while any(same_process(identity) for identity in identities) and time.monotonic() < cleanup_deadline:
                    time.sleep(0.02)
                for identity in reversed(identities):
                    if same_process(identity):
                        forced = True
                        try:
                            os.kill(identity["pid"], signal.SIGKILL)
                        except ProcessLookupError:
                            pass
            if process is not None:
                try:
                    process.wait(timeout=max(0.001, reservation.started + args.seconds - time.monotonic()))
                except subprocess.TimeoutExpired:
                    errors.append("direct child not reaped within reserved deadline")
            remaining = [identity for identity in identities if same_process(identity)]
            reaped = []
            for identity in identities:
                if process is not None and identity["pid"] == process.pid:
                    continue
                try:
                    pid, status = os.waitpid(identity["pid"], os.WNOHANG)
                    if pid:
                        reaped.append(dict(pid=pid, wait_status=status))
                    elif process_identity(identity["pid"]) is not None:
                        errors.append("adopted child not reaped")
                except ChildProcessError:
                    if process_identity(identity["pid"]) is not None:
                        errors.append("owned descendant still present")
            if watchdog_write is not None:
                try:
                    os.write(watchdog_write, b"D")
                except OSError as error:
                    errors.append("watchdog pipe: " + str(error))
                finally:
                    os.close(watchdog_write)
            if supervisor is not None:
                try:
                    supervisor.wait(timeout=max(0.001, reservation.started + args.seconds - time.monotonic()))
                    if supervisor.returncode:
                        errors.append("watchdog exited nonzero")
                except subprocess.TimeoutExpired:
                    errors.append("watchdog not reaped within deadline")
            signal.signal(signal.SIGTERM, previous_term)
            try:
                protection_after = verify_protected(repository)
                atomic_json(reservation.output / "protection-after.json", protection_after)
                if not protection_after["match"]:
                    errors.append("protected files differ after execution")
            except Exception as error:
                errors.append("protection verification: " + str(error))
            atomic_json(reservation.output / "cleanup.json", dict(forced=forced, errors=errors,
                        remaining=remaining, identities=identities, adopted_reaped=reaped,
                        direct_child_reaped=process is None or process.returncode is not None,
                        limits=["descendant acquisition requires live ancestry; daemon escape unsupported; workloads must not daemonize"]))
            if forced or remaining or errors:
                raise BudgetError("owned cleanup invalid")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--role", choices=("builder", "reviewer"), required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--kind", required=True)
    parser.add_argument("--seconds", type=float, required=True)
    parser.add_argument("--static-inventory-sha256")
    parser.add_argument("--static-inventory-name", default="static-inventory-v6.json")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.command[:1] == ["--"]:
        args.command.pop(0)
    if not args.command:
        parser.error("command is required")
    execute(args)


if __name__ == "__main__":
    main()
