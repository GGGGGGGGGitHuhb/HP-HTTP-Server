"""Independent absolute deadline guard; no workload or socket access."""
import json
import os
from pathlib import Path
import select
import signal
import sys
import time


def identity(pid):
    try:
        value = Path(f"/proc/{pid}/stat").read_text()
    except FileNotFoundError:
        return None
    fields = value[value.rfind(")") + 2:].split()
    return dict(pid=pid, starttime=int(fields[19]), ppid=int(fields[1]), state=fields[0], pgrp=int(fields[2]), session=int(fields[3]))


def alive(record):
    current = identity(record["pid"])
    return current is not None and current["starttime"] == record["starttime"]


def supervise(parent, starttime, soft_deadline, hard_deadline, pipe_fd, output):
    owner = dict(pid=parent, starttime=starttime)
    owned = {}
    terminated = False
    abnormal = False
    while True:
        now = time.monotonic()
        ready, _, _ = select.select([pipe_fd], [], [], min(0.02, max(0, hard_deadline - now)))
        if ready:
            message = os.read(pipe_fd, 1)
            if message == b"D":
                return
            if not message:
                abnormal = True
        candidates = []
        for path in Path("/proc").iterdir():
            if path.name.isdigit() and int(path.name) != os.getpid():
                record = identity(int(path.name))
                if record is not None:
                    candidates.append(record)
        parents = {parent} | {pid for pid, record in owned.items() if alive(record)}
        command_identity_path = Path(output).with_name("process.json")
        command_identity = json.loads(command_identity_path.read_text()) if command_identity_path.exists() else None
        # All workload descendants must remain in the newly created session.
        # This recovers siblings orphaned before an ancestry discovery poll.
        if command_identity and command_identity.get("session") == command_identity["pid"]:
            for record in candidates:
                if record["session"] == command_identity["session"] and record["starttime"] >= command_identity["starttime"]:
                    owned[record["pid"]] = record
                    parents.add(record["pid"])
        changed = True
        while changed:
            changed = False
            for record in candidates:
                if record["ppid"] in parents and record["pid"] not in parents:
                    parents.add(record["pid"])
                    owned[record["pid"]] = record
                    changed = True
        now = time.monotonic()
        if not alive(owner):
            abnormal = True
        if (now >= soft_deadline or abnormal) and not terminated:
            if alive(owner):
                try:
                    os.kill(parent, signal.SIGTERM)
                except ProcessLookupError:
                    pass
            for record in owned.values():
                if alive(record):
                    try:
                        os.kill(record["pid"], signal.SIGTERM)
                    except ProcessLookupError:
                        pass
            terminated = True
        if abnormal and not any(alive(record) for record in owned.values()):
            Path(output).write_text(json.dumps(dict(forced=False, parent_abnormal=True, identities=list(owned.values()), remaining=[])))
            return
        if time.monotonic() >= hard_deadline:
            forced = []
            for record in owned.values():
                if alive(record):
                    try:
                        os.kill(record["pid"], signal.SIGKILL)
                        forced.append(record)
                    except ProcessLookupError:
                        pass
            Path(output).write_text(json.dumps(dict(forced=True, identities=forced, parent_abnormal=abnormal, reason="absolute deadline")))
            if alive(owner):
                os.kill(parent, signal.SIGKILL)
            return


if __name__ == "__main__":
    supervise(int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4]), int(sys.argv[5]), sys.argv[6])
