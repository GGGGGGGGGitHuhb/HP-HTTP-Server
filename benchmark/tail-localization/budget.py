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
    for directory, folders, files in os.walk(root, followlinks=False):
        if any(Path(directory, name).is_symlink() for name in folders + files):
            raise BudgetError("output tree contains a symlink")
        for name in files:
            total += Path(directory, name).stat().st_size
    return total


class Reservation:
    def __init__(self, stage_root, role, run_id, kind, maximum_seconds, static_inventory_sha256=None):
        self.root = Path(stage_root).resolve()
        self.role = role
        self.run_id = run_id
        self.kind = kind
        self.maximum = maximum_seconds
        self.lock = None
        self.static_inventory_sha256 = static_inventory_sha256

    def output_bytes(self):
        total = 0
        role_root = self.root / self.role
        for directory, folders, files in os.walk(role_root, followlinks=False):
            if any(Path(directory, name).is_symlink() for name in folders):
                raise BudgetError("role tree contains symlink directory; not classified")
            for name in files:
                path = Path(directory, name)
                relative = str(path.relative_to(role_root))
                first = Path(relative).parts[0]
                if first.startswith("run-") or first in ("tmp", "cache") or relative in ("ledger.json", "ledger.pending", "static-inventory.json"):
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
        inventory_path = self.root / self.role / "static-inventory.json"
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
            inventory_path = self.root / self.role / "static-inventory.json"
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
            self.started = time.monotonic()
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
        size = self.output_bytes()
        if elapsed > self.maximum or size > self.authorization["per_role_output_bytes"]:
            raise BudgetError("runtime/output budget exceeded")
        return dict(elapsed_seconds=elapsed, bytes=size)

    def __exit__(self, exception_type, exception, traceback):
        try:
            elapsed = time.monotonic() - self.started
            size = self.output_bytes()
            self.verify_static_hashes()
            exceeded = elapsed > self.maximum or size > self.authorization["per_role_output_bytes"]
            self.record.update(status="invalid" if exception or exceeded else "valid", charged_seconds=elapsed,
                               role_output_bytes=size, output_attribution="all run directories + tmp/cache + ledger; sealed static source/build inventory excluded and separately disclosed", static_bytes=sum(item["size"] for item in self.static_inventory.values()), error=str(exception) if exception else ("budget exceeded" if exceeded else None))
            atomic_json(self.ledger_path, self.ledger)
        finally:
            self.lock.close()
        if not exception and exceeded:
            raise BudgetError("runtime/output budget exceeded")
