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


FIXTURE_ROOTS = ("build-E/test-tmp", "build-observed/test-tmp", "E-S3/source/.test-tmp", "E-observed/source/.test-tmp")

R008_FIXTURE_ROOTS = ("run-r008-build-002/server/.test-tmp", "run-r008-build-004/server/.test-tmp", "run-r018-build-001/build-output/build-B/test-tmp")

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


def scan_bytes(root, classify, allow_missing, disappearance=None, root_open_path=None):
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
                from r026_admission import sparse_charge
                special = sparse_charge(child, status, fd, name)
                total += classify(child_relative, child, status, None) if special is None else special
            else:
                raise BudgetError("non-regular output node")
        return total

    try:
        fd = os.open(root if root_open_path is None else root_open_path, flags)
    except FileNotFoundError as error:
        missing("", error)
        return 0
    try:
        return visit(fd, root, "")
    finally:
        os.close(fd)


def file_bytes(root, fixture_root=None, disappearance=None, fixture_roots=(), root_open_path=None):
    def classify(relative, path, status, target):
        if target is not None:
            boundary = fixture_root
            for candidate in fixture_roots:
                if path.is_relative_to(candidate): boundary = candidate
            return fixture_link_bytes(path, boundary, status, target)
        return status.st_size
    return scan_bytes(root, classify, lambda relative: True, disappearance, root_open_path)


from r016.r015_admission import check_slot, shared_package_bytes, capacity_groups, START_BYTES, control_debt, consume_recovery, claim_recovery, check_history_envelope


from r026_admission import check_slot as check_r018_slot, capacity as r018_capacity, new_debt


class Reservation:
    def __init__(self, stage_root, role, run_id, kind, maximum_seconds, static_inventory_sha256=None, static_inventory_name="static-inventory-v6.json", recovery_context=None):
        self.root = Path(stage_root).resolve()
        self.role = role
        self.run_id = run_id
        self.kind = kind
        self.maximum = maximum_seconds
        self.lock = None
        self.recovery_context = recovery_context
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
                relative.startswith("E-observed/source/.test-tmp/") or
                (parts[0] in ("build-E", "build-observed") and len(parts) > 1 and parts[1] in ("Testing", "test-tmp")))

    def note_disappearance(self, relative):
        self.dynamic_disappearance_count += 1

    def is_dynamic_path(self, relative):
        return relative == "" or self.dynamic_file(relative) or relative in FIXTURE_ROOTS

    def _original_dynamic_bytes(self):
        role_root = self.root / self.role
        total = 0
        # Only registered output branches; source/static material is excluded from periodic work.
        for name in os.listdir(role_root):
            if name.startswith("run-") or name in ("tmp", "cache"):
                total += file_bytes(role_root / name, disappearance=self.note_disappearance, fixture_roots=tuple(role_root / fixture for fixture in R008_FIXTURE_ROOTS))
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
            total += file_bytes(role_root / name, disappearance=self.note_disappearance, fixture_roots=tuple(role_root / fixture for fixture in R008_FIXTURE_ROOTS))
        for name in FIXTURE_ROOTS:
            root = role_root / name
            total += file_bytes(root, root, self.note_disappearance)
        return total

    def _original_output_bytes(self):
        role_root = self.root / self.role
        seen_static = set()
        def classify(relative, path, status, target):
            if self.dynamic_file(relative):
                if target is not None:
                    for name in FIXTURE_ROOTS + R008_FIXTURE_ROOTS:
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

    def r015_shared_bytes(self):
        return shared_package_bytes(self.root.parents[1]) if self.run_id.startswith("run-r015-") else 0

    def _r018_original_dynamic_bytes(self):
        size = self._original_dynamic_bytes() + self.r015_shared_bytes()
        if self.run_id.startswith("run-r015-"):
            try:
                capacity_groups(self.root, self.role, self.r015_shared_bytes())
            except ValueError as error:
                raise BudgetError(str(error)) from error
        return size

    def _r018_original_output_bytes(self):
        size = self._original_output_bytes() + self.r015_shared_bytes()
        if self.run_id.startswith("run-r015-"):
            try:
                capacity_groups(self.root, self.role, self.r015_shared_bytes())
            except ValueError as error:
                raise BudgetError(str(error)) from error
        return size

    def r018_upper_bytes(self, original):
        if not (self.run_id.startswith("run-r018-") or self.run_id == "run-r022-check-001"):
            return original
        try:
            upper = r018_capacity(self.root, self.role, self.authorization, scan_bytes, fixture_link_bytes)
        except ValueError as error:
            raise BudgetError(str(error)) from error
        return max(original, upper)

    def dynamic_bytes(self):
        return self.r018_upper_bytes(self._r018_original_dynamic_bytes())

    def output_bytes(self):
        return self.r018_upper_bytes(self._r018_original_output_bytes())

    def __enter__(self):
        self.started = time.monotonic()
        if self.role not in ("builder", "reviewer") or not re.fullmatch(r"run-[a-z0-9]+(?:-[a-z0-9]+)*", self.run_id):
            raise BudgetError("invalid role/run")
        if self.kind not in ("selfcheck", "offline", "sanitizer", "ctest", "smoke", "staircase", "observed_abba", "confirmation_ab", "r008_link", "baseline", "boundary"):
            raise BudgetError("unknown execution kind")
        self.lock = (self.root / "dynamic.lock").open("a+")
        try:
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            claim = claim_recovery(self.root, self.role, self.run_id)
            self.authorization = json.loads((self.root / "leader/authorization-r026.json").read_text())
            if self.authorization.get("roles_serial") is not True:
                raise BudgetError("missing serial authorization")
            if not 0 < self.maximum <= self.authorization["per_role_dynamic_seconds"]:
                raise BudgetError("invalid reservation")
            consume_recovery(self.root, self.role, self.run_id, self.authorization, None, self.recovery_context, claim)
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
                        receipt_authority="Approved S4 R002"
                        receipt_path=self.root / "leader/r002-current-builder-upper-bound.json"
                        if other_role == "builder" and run["run_id"] == "run-smoke-001":
                            receipt=self.authorization.get("r007_byte_reclassification")
                            receipt_authority="Approved S4 R007"
                            receipt_path=self.root / "leader/r007-current-builder-upper-bound.json"
                        reconciled = False
                        if receipt:
                            if receipt_path.exists() and hashlib.sha256(receipt_path.read_bytes()).hexdigest() == receipt["sha256"]:
                                value = json.loads(receipt_path.read_text())
                                reconciled = (receipt.get("run_id") == run["run_id"] and receipt.get("role") == "builder"
                                    and value.get("authority") == receipt_authority and value.get("scope") == str(self.root / "builder")
                                    and value.get("bytes_upper_bound", float("inf")) < self.authorization["per_role_output_bytes"])
                        if not reconciled:
                            raise BudgetError("unreconciled unknown bytes stop new execution")
            if any(run["run_id"] == self.run_id for run in self.ledger["runs"]):
                raise BudgetError("run id already exists")
            if self.role == "builder" and self.kind == "ctest" and self.run_id.startswith("run-baseline-ctest-"):
                if sum(run["run_id"].startswith("run-baseline-ctest-") for run in self.ledger["runs"]) >= 3:
                    raise BudgetError("R003 forbids a fourth baseline CTest attempt")
            check_history_envelope(self.role, self.ledger)
            debt = control_debt(self.root, self.role, self.authorization)
            if self.run_id.startswith("run-r018-") or self.run_id == "run-r022-check-001":
                debt += new_debt(self.root,self.role,self.authorization)
            charged = sum(run.get("charged_seconds", run["reserved_seconds"]) for run in self.ledger["runs"]) + debt
            if charged + self.maximum > self.authorization["per_role_dynamic_seconds"]:
                raise BudgetError("dynamic seconds exhausted")
            r008 = self.authorization.get("r008")
            if r008:
                starting = r008.get(self.role + "_start_charged_seconds", 0)
                r008_runs = [run for run in self.ledger["runs"] if run["run_id"].startswith("run-r008-")]
                r008_charged = sum(run.get("charged_seconds",run["reserved_seconds"]) for run in r008_runs)
                if self.run_id.startswith("run-r008-"):
                    if r008_charged + self.maximum > r008[self.role + "_subbudget_seconds"]:
                        raise BudgetError("R008 role subbudget exhausted")
                    if self.role == "builder" and charged + self.maximum + r008["builder_m3_reserved_seconds"] > self.authorization["per_role_dynamic_seconds"]:
                        raise BudgetError("R008 would consume M3 reserve")
                if self.kind == "r008_link":
                    slots = ["run-" + suffix for suffix in r008["per_role_samples"]]
                    if self.run_id not in slots or self.maximum != r008["sample_outer_seconds"]:
                        raise BudgetError("unapproved R008 sample/deadline")
                    position = slots.index(self.run_id)
                    for prior in slots[:position]:
                        matches = [run for run in self.ledger["runs"] if run["run_id"] == prior]
                        if len(matches) != 1 or matches[0]["status"] != "valid":
                            raise BudgetError("prior R008 sample not valid")
                        prior_output=self.root / self.role / prior
                        receipt_path=prior_output / "association.json"
                        if not receipt_path.is_file() or receipt_path.is_symlink():
                            raise BudgetError("prior R008 association receipt missing")
                        receipt=json.loads(receipt_path.read_text())
                        sample_path=prior_output / "sample.json"
                        if not sample_path.is_file() or sample_path.is_symlink():
                            raise BudgetError("prior R008 sample binding missing")
                        sample=json.loads(sample_path.read_text())
                        position_prior=slots.index(prior)
                        expected_connections=(2,2,8,128)[position_prior]
                        expected_quota=(1,16,16,0)[position_prior]
                        expected_scope="process-local" if position_prior<2 else "kernel-mapped"
                        expected_selected=expected_connections if position_prior<3 else 4
                        cleanup=dict(forced=False,errors=[],remaining=[])
                        manifest=sample.get("manifest",{})
                        parameters=sample.get("parameters",{})
                        manifest_sha=hashlib.sha256(json.dumps(manifest,sort_keys=True,separators=(",",":")).encode()).hexdigest()
                        if not (receipt.get("schema")==3 and receipt.get("status")=="valid"
                            and receipt.get("run_id")==prior and receipt.get("role")==self.role
                            and manifest.get("role")==self.role and sample.get("status")=="valid"
                            and receipt.get("sample_sha256")==hashlib.sha256(sample_path.read_bytes()).hexdigest()
                            and receipt.get("manifest_sha256")==manifest_sha
                            and receipt.get("parameters")==parameters
                            and parameters.get("connections")==expected_connections
                            and parameters.get("requests_per_connection")==expected_quota
                            and parameters.get("detailed") is True
                            and receipt.get("identity_scope")==sample.get("identity_scope")==expected_scope
                            and receipt.get("kernel_mapping_verified") is (position_prior>=2)
                            and receipt.get("selected_connections")==len(sample.get("frozen_connections",[]))==expected_selected
                            and receipt.get("overflow") is False
                            and receipt.get("cleanup")==sample.get("cleanup")==cleanup
                            and isinstance(receipt.get("complete_requests"),int) and receipt["complete_requests"]>0
                            and (not expected_quota or (receipt["complete_requests"]==expected_connections*expected_quota and receipt.get("boundaries")==[]))):
                            raise BudgetError("prior R008 association binding/count/scope invalid")
                        catalog=receipt.get("file_catalog",[])
                        expected_raw={str(prior_output / f"{endpoint}-worker-{worker}.events.bin") for endpoint,total in (("server",4),("client",2)) for worker in range(total)}
                        if len(catalog)!=6 or {entry.get("path") for entry in catalog}!=expected_raw:
                            raise BudgetError("prior R008 raw catalog missing/duplicate")
                        for entry in catalog:
                            raw=Path(entry.get("path",""))
                            if (not raw.is_absolute() or ".." in raw.parts or not raw.is_relative_to(prior_output) or not raw.is_file()
                                or any(parent.is_symlink() for parent in (raw,*raw.parents))
                                or not isinstance(entry.get("records"),int) or entry["records"]<0
                                or entry.get("identity_scope")!=expected_scope
                                or hashlib.sha256(raw.read_bytes()).hexdigest()!=entry.get("sha256")):
                                raise BudgetError("prior R008 raw catalog binding invalid")
                    if any(run["run_id"] in slots[position+1:] for run in self.ledger["runs"]):
                        raise BudgetError("R008 sequence drift")
            role_root = self.root / self.role
            if self.output_bytes() >= self.authorization["per_role_output_bytes"]:
                raise BudgetError("output budget exhausted")
            try:
                is_r015 = check_slot(self.role, self.run_id, self.kind, self.maximum, self.authorization,
                                     self.ledger["runs"], self._original_output_bytes(), self.r015_shared_bytes())
            except ValueError as error:
                raise BudgetError(str(error)) from error
            try:
                is_r018 = check_r018_slot(self.root, self.role, self.run_id, self.kind, self.maximum, self.authorization)
            except ValueError as error:
                raise BudgetError(str(error)) from error
            limits = self.authorization[self.role + "_http_limits"]
            if not is_r015 and not is_r018 and self.kind in limits and sum(run["kind"] == self.kind for run in self.ledger["runs"]) >= limits[self.kind]:
                raise BudgetError("sample slots exhausted")
            self.output = role_root / self.run_id
            self.output.mkdir(exist_ok=False)
            self.record = dict(run_id=self.run_id, kind=self.kind, status="running", reserved_seconds=self.maximum,
                               start_monotonic=self.started, output=str(self.output), old_s3=self.authorization["old_s3_ledgers"][self.role])
            if self.run_id == 'run-r022-check-001' or (self.role == 'builder' and self.run_id == 'run-r018-build-001'):
                from r026_admission import get_claim
                current_claim = get_claim()
                self.record['r019_called_sha256'] = current_claim['called_sha256']
                self.record['r019_used_path'] = current_claim['path']
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
                from r026_admission import SPARSE_EVIDENCE
                self.record['sparse_accounting_evidence'] = list(SPARSE_EVIDENCE)
                atomic_json(self.ledger_path, self.ledger)
            finally:
                self.lock.close()
        if not exception and (exceeded or accounting_errors):
            raise BudgetError("settled invalid: runtime/output/accounting failure")
