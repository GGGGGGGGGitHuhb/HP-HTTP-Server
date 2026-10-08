"""Durable S4 reservations. No S3 ledger is opened for writing."""
import fcntl
import hashlib
import json
import os
import re
import stat
from pathlib import Path
import time


class BudgetError(RuntimeError):
    pass


def atomic_json(path, value):
    temporary = path.with_suffix(".pending")
    with temporary.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(path)
    descriptor = os.open(path.parent, os.O_DIRECTORY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def file_bytes(root):
    total = 0
    if root.is_symlink():
        raise BudgetError("dynamic root is a symlink")
    for directory, folders, files in os.walk(root, followlinks=False):
        if any(Path(directory, name).is_symlink() for name in folders + files):
            raise BudgetError("output tree contains a symlink")
        for name in files:
            status = Path(directory, name).lstat()
            if not stat.S_ISREG(status.st_mode):
                raise BudgetError("dynamic file is not regular")
            total += status.st_size
    return total


class Reservation:
    def __init__(self, stage_root, role, run_id, kind, maximum_seconds, static_inventory_sha256=None, static_inventory_name="static-inventory-v2.json"):
        self.root = Path(stage_root).resolve()
        self.role = role
        self.run_id = run_id
        self.kind = kind
        self.maximum = maximum_seconds
        self.lock = None
        self.static_inventory_sha256 = static_inventory_sha256
        if not re.fullmatch(r"static-inventory-v[0-9]+\.json", static_inventory_name):
            raise BudgetError("invalid inventory version name")
        self.static_inventory_name = static_inventory_name
        self.last_guard_time = -1
        self.last_guard_bytes = 0

    def dynamic_file(self, relative):
        parts = Path(relative).parts
        return (parts[0].startswith("run-") or parts[0] in ("tmp", "cache") or
                relative in ("ledger.json", "ledger.pending", "static-inventory.json") or
                bool(re.fullmatch(r"static-inventory-v[0-9]+\.json", relative)) or
                (parts[0] in ("build-E", "build-observed") and len(parts) > 1 and parts[1] in ("Testing", "test-tmp")))

    def dynamic_bytes(self):
        role_root = self.root / self.role
        roots = [path for path in role_root.iterdir() if path.name.startswith("run-") and path.is_dir()]
        roots += [role_root / name for name in ("tmp", "cache", "build-E/Testing", "build-E/test-tmp", "build-observed/Testing", "build-observed/test-tmp")]
        total = sum(file_bytes(root) for root in roots if root.exists())
        for path in role_root.iterdir():
            if path.is_file() and self.dynamic_file(path.name):
                if not stat.S_ISREG(path.lstat().st_mode):
                    raise BudgetError("non-regular dynamic metadata")
                total += path.stat().st_size
        return total

    def output_bytes(self):
        total = 0
        role_root = self.root / self.role
        for directory, folders, files in os.walk(role_root, followlinks=False):
            if any(Path(directory, name).is_symlink() for name in folders):
                raise BudgetError("role tree contains symlink directory; not classified")
            for name in files:
                path = Path(directory, name)
                relative = str(path.relative_to(role_root))
                if self.dynamic_file(relative):
                    if not stat.S_ISREG(path.lstat().st_mode):
                        raise BudgetError("dynamic output must be a regular file")
                    total += path.stat().st_size
                    continue
                expected = self.static_inventory.get(relative)
                status = path.lstat()
                if expected is None or status.st_size != expected["size"] or status.st_mtime_ns != expected["mtime_ns"]:
                    raise BudgetError("unknown/modified file outside dynamic roots: " + relative)
                if path.is_symlink() and (expected.get("type") != "symlink" or os.readlink(path) != expected["target"]):
                    raise BudgetError("static symlink drift")
        return total

    def verify_static_hashes(self):
        inventory_path = self.root / self.role / self.static_inventory_name
        if inventory_path.exists() and hashlib.sha256(inventory_path.read_bytes()).hexdigest() != self.static_inventory_sha256:
            raise BudgetError("static inventory differs from sealed command hash")
        for relative, expected in self.static_inventory.items():
            path = self.root / self.role / relative
            if not path.absolute().is_relative_to(self.root / self.role) or ".." in Path(relative).parts:
                raise BudgetError("unsafe static inventory path")
            if expected.get("type") == "symlink":
                if not path.is_symlink() or os.readlink(path) != expected["target"]:
                    raise BudgetError("static symlink drift")
                continue
            if path.is_symlink():
                raise BudgetError("unexpected symlink")
            digest = hashlib.sha256()
            with path.open("rb") as source:
                for block in iter(lambda: source.read(1024 * 1024), b""):
                    digest.update(block)
            if digest.hexdigest() != expected["sha256"]:
                raise BudgetError("static input/build hash drift")

    def __enter__(self):
        self.started = time.monotonic()
        if self.role not in ("builder", "reviewer") or not re.fullmatch(r"run-[a-z0-9]+(?:-[a-z0-9]+)*", self.run_id):
            raise BudgetError("invalid role/run")
        if self.kind not in ("selfcheck", "offline", "sanitizer", "ctest", "smoke", "staircase", "observed_abba", "confirmation_ab"):
            raise BudgetError("unknown execution kind")
        self.authorization = json.loads((self.root / "leader/authorization.json").read_text())
        if self.authorization.get("roles_serial") is not True:
            raise BudgetError("missing serial authorization")
        if not 0 < self.maximum <= self.authorization["per_role_dynamic_seconds"]:
            raise BudgetError("invalid reservation")
        self.lock = (self.root / "dynamic.lock").open("a+")
        try:
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.ledger_path = self.root / self.role / "ledger.json"
            self.ledger_path.parent.mkdir(parents=True, exist_ok=True)
            inventory_path = self.root / self.role / self.static_inventory_name
            if inventory_path.exists() and not self.static_inventory_sha256:
                raise BudgetError("static inventory requires hash in independently sealed command")
            self.static_inventory = json.loads(inventory_path.read_text())["files"] if inventory_path.exists() else {}
            self.verify_static_hashes()
            self.ledger = json.loads(self.ledger_path.read_text()) if self.ledger_path.exists() else dict(schema=1, role=self.role, runs=[])
            # Unsettled runs retain their full reservation and block automatic retry.
            for other_role in ("builder", "reviewer"):
                path = self.root / other_role / "ledger.json"
                if path.exists() and any(run["status"] == "running" for run in json.loads(path.read_text())["runs"]):
                    raise BudgetError("unsettled run requires Leader reconciliation")
            if any(run["run_id"] == self.run_id for run in self.ledger["runs"]):
                raise BudgetError("run id already exists")
            charged = sum(run.get("charged_seconds", run["reserved_seconds"]) for run in self.ledger["runs"])
            if charged + self.maximum > self.authorization["per_role_dynamic_seconds"]:
                raise BudgetError("dynamic seconds exhausted")
            role_root = self.root / self.role
            if self.output_bytes() >= self.authorization["per_role_output_bytes"]:
                raise BudgetError("output budget exhausted")
            limits = self.authorization[self.role + "_http_limits"]
            if self.kind in limits and sum(run["kind"] == self.kind for run in self.ledger["runs"]) >= limits[self.kind]:
                raise BudgetError("sample slots exhausted")
            self.output = role_root / self.run_id
            self.output.mkdir(exist_ok=False)
            self.record = dict(run_id=self.run_id, kind=self.kind, status="running", reserved_seconds=self.maximum,
                               start_monotonic=self.started, output=str(self.output), old_s3=self.authorization["old_s3_ledgers"][self.role])
            self.ledger["runs"].append(self.record)
            atomic_json(self.ledger_path, self.ledger)
            return self
        except BaseException:
            self.lock.close()
            raise

    def guard(self):
        elapsed = time.monotonic() - self.started
        now = time.monotonic()
        if now - self.last_guard_time >= 0.25:
            self.last_guard_bytes = self.dynamic_bytes()
            self.last_guard_time = now
        size = self.last_guard_bytes
        if elapsed > self.maximum or size > self.authorization["per_role_output_bytes"] - 8 * 1024 * 1024:
            raise BudgetError("runtime/output budget exceeded")
        return dict(elapsed_seconds=elapsed, bytes=size)

    def __exit__(self, exception_type, exception, traceback):
        try:
            elapsed = time.monotonic() - self.started
            size = self.output_bytes()
            self.verify_static_hashes()
            exceeded = elapsed > self.maximum or size > self.authorization["per_role_output_bytes"]
            self.record.update(status="invalid" if exception or exceeded else "valid", charged_seconds=elapsed,
                               role_output_bytes=size, output_attribution="all runs + tmp/cache + build Testing/test-tmp + ledger; sealed static source/build inventory excluded", guard_interval_seconds=0.25, stop_headroom_bytes=8 * 1024 * 1024, static_bytes=sum(item["size"] for item in self.static_inventory.values()), error=str(exception) if exception else ("budget exceeded" if exceeded else None))
            atomic_json(self.ledger_path, self.ledger)
        finally:
            self.lock.close()
        if not exception and exceeded:
            raise BudgetError("runtime/output budget exceeded")
