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


FIXTURE_ROOTS = ("build-E/test-tmp", "build-observed/test-tmp", "E-S3/source/.test-tmp")


def fixture_link_bytes(path, fixture_root, status=None, target=None):
    if fixture_root is None:
        raise BudgetError("symlink outside explicitly registered testcase tree")
    parts = path.relative_to(fixture_root).parts
    if not parts or not re.fullmatch(r"(?:integration|static)-[A-Za-z0-9]{6}", parts[0]):
        raise BudgetError("unknown testcase symlink")
    case = fixture_root / parts[0]
    suffix = "/".join(parts[1:])
    allowed = {"root/escape.txt": case / "sibling-secret.txt"}
    if parts[0].startswith("static-"):
        allowed.update({"root/escape-dir": case, "root-link": case / "root"})
    if status is None:
        status = path.lstat()
    if target is None:
        target = os.readlink(path)
    # Lexical identity only: scanner opened every ancestor O_NOFOLLOW; never read link target.
    normalized = Path(os.path.abspath(os.path.join(path.parent, target)))
    if suffix not in allowed or normalized != allowed[suffix].absolute() or not normalized.is_relative_to(case.absolute()):
        raise BudgetError("fixture symlink target/path outside original testcase seam")
    return status.st_size


def scan_bytes(root, classify, allow_missing, disappearance=None):
    """Relative FD scan: no directory symlink can be traversed after an exchange."""
    root = Path(root)
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC

    def missing(relative, error):
        if not isinstance(error, FileNotFoundError) or not allow_missing(relative):
            raise error
        if disappearance is not None:
            disappearance(relative)

    def visit(fd, directory, relative):
        total = 0
        for name in os.listdir(fd):
            child_relative = str(Path(relative) / name) if relative else name
            child = directory / name
            try:
                status = os.stat(name, dir_fd=fd, follow_symlinks=False)
            except FileNotFoundError as error:
                missing(child_relative, error)
                continue
            if stat.S_ISDIR(status.st_mode):
                try:
                    child_fd = os.open(name, flags, dir_fd=fd)
                except FileNotFoundError as error:
                    missing(child_relative, error)
                    continue
                try:
                    opened = os.fstat(child_fd)
                    if (status.st_dev, status.st_ino) != (opened.st_dev, opened.st_ino):
                        raise BudgetError("directory identity exchanged during scan")
                    total += visit(child_fd, child, child_relative)
                finally:
                    os.close(child_fd)
            elif stat.S_ISLNK(status.st_mode):
                try:
                    target = os.readlink(name, dir_fd=fd)
                except FileNotFoundError as error:
                    missing(child_relative, error)
                    continue
                total += classify(child_relative, child, status, target)
            elif stat.S_ISREG(status.st_mode):
                total += classify(child_relative, child, status, None)
            else:
                raise BudgetError("non-regular output node")
        return total

    try:
        fd = os.open(root, flags)
    except FileNotFoundError as error:
        missing("", error)
        return 0
    try:
        return visit(fd, root, "")
    finally:
        os.close(fd)


def file_bytes(root, fixture_root=None, disappearance=None):
    def classify(relative, path, status, target):
        if target is not None:
            return fixture_link_bytes(path, fixture_root, status, target)
        return status.st_size
    return scan_bytes(root, classify, lambda relative: True, disappearance)


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
        self.dynamic_disappearance_count = 0

    def dynamic_file(self, relative):
        parts = Path(relative).parts
        return (parts[0].startswith("run-") or parts[0] in ("tmp", "cache") or
                relative in ("ledger.json", "ledger.pending", "static-inventory.json") or
                bool(re.fullmatch(r"static-inventory-v[0-9]+\.json", relative)) or
                relative.startswith("E-S3/source/.test-tmp/") or
                (parts[0] in ("build-E", "build-observed") and len(parts) > 1 and parts[1] in ("Testing", "test-tmp")))

    def note_disappearance(self, relative):
        self.dynamic_disappearance_count += 1

    def is_dynamic_path(self, relative):
        return relative == "" or self.dynamic_file(relative) or relative in FIXTURE_ROOTS

    def dynamic_bytes(self):
        role_root = self.root / self.role
        total = 0
        # Only registered output branches; source/static material is excluded from periodic work.
        for name in os.listdir(role_root):
            if name.startswith("run-") or name in ("tmp", "cache"):
                total += file_bytes(role_root / name, disappearance=self.note_disappearance)
            elif self.dynamic_file(name):
                try:
                    status = (role_root / name).lstat()
                except FileNotFoundError:
                    self.note_disappearance(name)
                    continue
                if not stat.S_ISREG(status.st_mode):
                    raise BudgetError("non-regular dynamic metadata")
                total += status.st_size
        for name in ("build-E/Testing", "build-observed/Testing"):
            total += file_bytes(role_root / name, disappearance=self.note_disappearance)
        for name in FIXTURE_ROOTS:
            root = role_root / name
            total += file_bytes(root, root, self.note_disappearance)
        return total

    def output_bytes(self):
        role_root = self.root / self.role
        seen_static = set()
        def classify(relative, path, status, target):
            if self.dynamic_file(relative):
                if target is not None:
                    for name in FIXTURE_ROOTS:
                        fixture_root = role_root / name
                        if path.is_relative_to(fixture_root):
                            return fixture_link_bytes(path, fixture_root, status, target)
                    raise BudgetError("ordinary raw symlink rejected")
                return status.st_size
            expected = self.static_inventory.get(relative)
            if expected is None or status.st_size != expected["size"] or status.st_mtime_ns != expected["mtime_ns"]:
                raise BudgetError("unknown/modified file outside dynamic roots: " + relative)
            if (target is not None) != (expected.get("type") == "symlink"):
                raise BudgetError("static node type drift")
            if target is not None and target != expected["target"]:
                raise BudgetError("static symlink drift")
            seen_static.add(relative)
            return 0
        total = scan_bytes(role_root, classify, self.is_dynamic_path, self.note_disappearance)
        if seen_static != set(self.static_inventory):
            raise BudgetError("static material missing from role tree")
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
                               dynamic_disappearance_count=self.dynamic_disappearance_count,
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
