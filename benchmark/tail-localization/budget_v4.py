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


def fixture_link_bytes(path, fixture_root):
    if fixture_root is None:
        raise BudgetError("symlink outside explicitly registered testcase tree")
    relative = path.relative_to(fixture_root)
    parts = relative.parts
    if not parts or not re.fullmatch(r"(?:integration|static)-[A-Za-z0-9]{6}", parts[0]):
        raise BudgetError("unknown testcase symlink")
    case = fixture_root / parts[0]
    if case.is_symlink():
        raise BudgetError("testcase root is a symlink")
    suffix = "/".join(parts[1:])
    allowed = {"root/escape.txt": case / "sibling-secret.txt"}
    if parts[0].startswith("static-"):
        allowed.update({"root/escape-dir": case, "root-link": case / "root"})
    if suffix not in allowed or path.resolve() != allowed[suffix].resolve() or not path.resolve().is_relative_to(case):
        raise BudgetError("fixture symlink target/path outside original testcase seam")
    return path.lstat().st_size


def file_bytes(root, fixture_root=None):
    total = 0
    if root.is_symlink():
        raise BudgetError("dynamic root is a symlink")
    for directory, folders, files in os.walk(root, followlinks=False):
        kept = []
        for name in folders:
            path = Path(directory, name)
            if path.is_symlink():
                total += fixture_link_bytes(path, fixture_root)
            else:
                kept.append(name)
        folders[:] = kept
        for name in files:
            path = Path(directory, name)
            if path.is_symlink():
                total += fixture_link_bytes(path, fixture_root)
                continue
            status = path.lstat()
            if not stat.S_ISREG(status.st_mode):
                raise BudgetError("dynamic file is not regular")
            total += status.st_size
    return total


class Reservation:
    def __init__(self, stage_root, role, run_id, kind, maximum_seconds, static_inventory_sha256=None, static_inventory_name="static-inventory-v4.json"):
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
                relative.startswith("E-S3/source/.test-tmp/") or
                (parts[0] in ("build-E", "build-observed") and len(parts) > 1 and parts[1] in ("Testing", "test-tmp")))

    def dynamic_bytes(self):
        role_root = self.root / self.role
        roots = [path for path in role_root.iterdir() if path.name.startswith("run-") and path.is_dir()]
        roots += [role_root / name for name in ("tmp", "cache", "build-E/Testing", "build-E/test-tmp", "build-observed/Testing", "build-observed/test-tmp", "E-S3/source/.test-tmp")]
        total = sum(file_bytes(root, root if (root.name == "test-tmp" and root.parent.name in ("build-E", "build-observed")) or root == role_root / "E-S3/source/.test-tmp" else None) for root in roots if root.exists())
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
            relative_directory = Path(directory).relative_to(role_root)
            if str(relative_directory) in ("build-E/test-tmp", "build-observed/test-tmp", "E-S3/source/.test-tmp"):
                total += file_bytes(Path(directory), Path(directory))
                folders[:] = []
                continue
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
                if path.exists():
                    for run in json.loads(path.read_text())["runs"]:
                        if run["status"] == "running":
                            raise BudgetError("unsettled run requires Leader reconciliation")
                        if run.get("byte_classification_status") != "unknown":
                            continue
                        receipt = self.authorization.get("r002_byte_reclassification") if other_role == "builder" and run["run_id"] == "run-baseline-ctest-001" else None
                        reconciled = False
                        if receipt:
                            receipt_path = self.root / "leader/r002-current-builder-upper-bound.json"
                            if receipt_path.exists() and hashlib.sha256(receipt_path.read_bytes()).hexdigest() == receipt["sha256"]:
                                value = json.loads(receipt_path.read_text())
                                reconciled = (receipt.get("run_id") == run["run_id"] and receipt.get("role") == "builder"
                                    and value.get("authority") == "Approved S4 R002" and value.get("scope") == str(self.root / "builder")
                                    and value.get("bytes_upper_bound", float("inf")) < self.authorization["per_role_output_bytes"])
                        if not reconciled:
                            raise BudgetError("unreconciled unknown bytes stop new execution")
            if any(run["run_id"] == self.run_id for run in self.ledger["runs"]):
                raise BudgetError("run id already exists")
            if self.role == "builder" and self.kind == "ctest" and self.run_id.startswith("run-baseline-ctest-"):
                if sum(run["run_id"].startswith("run-baseline-ctest-") for run in self.ledger["runs"]) >= 3:
                    raise BudgetError("R003 forbids a fourth baseline CTest attempt")
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
        accounting_errors = []
        size = None
        elapsed = time.monotonic() - self.started
        try:
            try:
                size = self.output_bytes()
            except Exception as error:
                accounting_errors.append("byte classification: " + str(error))
            try:
                self.verify_static_hashes()
            except Exception as error:
                accounting_errors.append("static verification: " + str(error))
        finally:
            elapsed = time.monotonic() - self.started
            exceeded = elapsed > self.maximum or (size is not None and size > self.authorization["per_role_output_bytes"])
            error = str(exception) if exception else ("budget exceeded" if exceeded else None)
            self.record.update(status="invalid" if exception or exceeded or accounting_errors else "valid",
                               charged_seconds=max(self.maximum, elapsed) if accounting_errors else elapsed,
                               observed_elapsed_seconds=elapsed, role_output_bytes=size,
                               byte_classification_status="unknown" if size is None else "verified",
                               accounting_errors=accounting_errors, error=error,
                               output_attribution="all runs + tmp/cache + build Testing/test-tmp + exact E-S3/source/.test-tmp + ledger; fixture links lstat nofollow; sealed static source/build separate",
                               guard_interval_seconds=0.25, stop_headroom_bytes=8 * 1024 * 1024,
                               static_bytes=sum(item["size"] for item in self.static_inventory.values()))
            try:
                atomic_json(self.ledger_path, self.ledger)
            finally:
                self.lock.close()
        if not exception and (exceeded or accounting_errors):
            raise BudgetError("settled invalid: runtime/output/accounting failure")
