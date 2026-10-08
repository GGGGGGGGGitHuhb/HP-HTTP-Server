"""R015 外治理真实准入函数反例；由具名 check60 计费执行。"""
import importlib.util
from pathlib import Path
import sys
import hashlib
import json
import tempfile
from types import SimpleNamespace
from unittest.mock import patch
import unittest
sys.dont_write_bytecode = True
TOOL = Path(__file__).absolute().parent.parent
REPOSITORY = TOOL.parents[1]
CLOSURE = [TOOL / name for name in ("budget_v12.py", "localize_v14.py", "protection.py", "proc_identity_v11.py", "cleanup_v11.py", "watchdog_v11.py")]
CLOSURE += [Path(__file__).absolute(), TOOL / "r016/r015_admission.py", TOOL / "r016/__init__.py"]
for source in CLOSURE:
    compile(source.read_bytes(), str(source), "exec")

def load(name, source, package=False):
    spec = importlib.util.spec_from_file_location(name, source, submodule_search_locations=[str(source.parent)] if package else None)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module

load("r016", TOOL / "r016/__init__.py", True)
admission = load("r016.r015_admission", TOOL / "r016/r015_admission.py")
budget = load("budget_v12_under_test", TOOL / "budget_v12.py")

class AdmissionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="r016-contract-")
        self.addCleanup(self.temporary.cleanup)
        self.stage = Path(self.temporary.name)
        self.authorization = {"per_role_output_bytes": 2 * 1024**3,
            "r015": {"revision": 1, "no_automatic_retries": True,
                     "per_role_output_bytes": 512*1024**2, "build_output_bytes": 320*1024**2,
                     "baseline_output_bytes": 128*1024**2, "other_output_bytes": 64*1024**2}}

    def check(self, run="run-r015-build-001", kind="selfcheck", seconds=180, rows=(), role="builder", total=1486288348):
        return admission.check_slot(role, run, kind, seconds, self.authorization, list(rows), total, 16*1024**2)

    def valid(self, run):
        return self.produced(run)

    def produced(self, run):
        # Only external clock/measurement seams are replaced; __exit__ writes its own canonical fields.
        role_root = self.stage / "builder"
        role_root.mkdir(exist_ok=True)
        output = role_root / run
        output.mkdir(exist_ok=True)
        producer = budget.Reservation(self.stage, "builder", run, admission.SLOTS[run][0], admission.SLOTS[run][1])
        producer.started = 100.0
        producer.authorization = {"per_role_output_bytes": 2 * 1024**3}
        producer.output = output
        producer.ledger_path = role_root / "ledger.json"
        producer.lock = (self.stage / "fixture.lock").open("a+")
        producer.static_inventory = {}
        producer.record = dict(run_id=run, kind=admission.SLOTS[run][0], status="running",
                               reserved_seconds=admission.SLOTS[run][1], output=str(output))
        producer.ledger = dict(schema=1, role="builder", runs=[producer.record])
        if run == "run-r015-build-001":
            frozen = REPOSITORY / ".cache/v0.5.1-s4/builder/run-r015-build-001/build-output/build-receipt.json"
            destination = output / "build-output"
            destination.mkdir(exist_ok=True)
            (destination / "build-receipt.json").write_bytes(frozen.read_bytes())
            admission.record_build_artifact(SimpleNamespace(run_id=run), producer,
                {"HP_BASELINE_PACKAGE_SHA256": "cc610aab7e2f5f962c86a82a543dabcda25211bf23d4cf47e496283f739879af"})
        with patch.object(producer, "output_bytes", return_value=1234), \
             patch.object(producer, "verify_static_hashes", return_value=None), \
             patch.object(budget.time, "monotonic", return_value=101.0):
            producer.__exit__(None, None, None)
        actual = json.loads(producer.ledger_path.read_text())
        admission.check_history_envelope("builder", actual)
        return actual["runs"][0]

    def test_real_settlement_and_frozen_build(self):
        row = self.produced("run-r015-build-001")
        self.assertEqual(row["byte_classification_status"], "verified")
        self.assertNotIn("byte_accounting", row)
        self.assertTrue(self.check(run="run-r015-check-001", seconds=60, rows=[row]))
        ledger = json.loads((REPOSITORY / ".cache/v0.5.1-s4/builder/ledger.json").read_text())
        admission.check_history_envelope("builder", ledger)
        frozen = [item for item in ledger["runs"] if item["run_id"] == "run-r015-build-001"]
        self.assertEqual(len(frozen), 1)
        receipt = REPOSITORY / ".cache/v0.5.1-s4/builder/run-r015-build-001/build-output/build-receipt.json"
        self.assertEqual(hashlib.sha256(receipt.read_bytes()).hexdigest(), frozen[0]["r015_build_receipt_sha256"])
        self.assertEqual(frozen[0]["r015_package_sha256"], row["r015_package_sha256"])
        self.assertTrue(self.check(run="run-r015-check-001", seconds=60, rows=frozen))
        isolated = self.stage / "frozen-build-row.json"
        isolated.write_text(json.dumps(frozen[0]))
        self.assertEqual(json.loads(isolated.read_text()), frozen[0])

    def test_real_record_canonical_key_types_and_paths(self):
        row = self.produced("run-r015-build-001")
        wrong = dict(row)
        del wrong["byte_classification_status"]
        wrong["byte_accounting"] = "verified"
        with self.assertRaises(ValueError): self.check(run="run-r015-check-001", seconds=60, rows=[wrong])
        for key, value in (("byte_classification_status", True), ("accounting_errors", {}),
                           ("status", "invalid"), ("reserved_seconds", True), ("charged_seconds", float("nan")),
                           ("output", str(self.stage / "reviewer/run-r015-build-001")),
                           ("output", str(self.stage / "builder/wrong-run"))):
            damaged = dict(row, **{key: value})
            with self.assertRaises(ValueError): self.check(run="run-r015-check-001", seconds=60, rows=[damaged])
        with self.assertRaises(ValueError): admission.check_history_envelope("builder", dict(role="reviewer", runs=[row]))

    def frozen_row(self):
        ledger = json.loads((REPOSITORY / ".cache/v0.5.1-s4/builder/ledger.json").read_text())
        return next(row for row in ledger["runs"] if row["run_id"] == "run-r015-build-001")

    def test_actual_artifact_consumer_refuses_sha_drift(self):
        stage = REPOSITORY / ".cache/v0.5.1-s4"
        role = stage / "builder"
        primary = role / "cache/measurement-baseline-r015"
        test = TOOL / "r016/test_r015_adapter.py"
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        command = ["/usr/bin/python3", "-I", str(primary / "check_package.py"), "--package-root", str(primary),
                   "--build-output", str(role / "run-r015-build-001/build-output"), "--output-root", str(role / "run-r015-check-001"),
                   "--governance-test", str(test), "--governance-test-sha256", sha(test),
                   "--governance-module-sha256", sha(test.with_name("r015_admission.py"))]
        args = SimpleNamespace(role="builder", run_id="run-r015-check-001", seconds=60, command=command,
                               r015_package_sha256=sha(primary / "inputs-lock.json"))
        row = self.frozen_row()
        reservation = SimpleNamespace(root=stage, output=role / args.run_id, started=100.0, ledger={"runs": [row]})
        admission.prepare_environment(args, reservation, REPOSITORY, {}, {"pid": 9001, "starttime": 10})
        for key in ("r015_package_sha256", "r015_build_receipt_sha256"):
            reservation.ledger = {"runs": [dict(row, **{key: "0" * 64})]}
            with self.assertRaises(ValueError): admission.prepare_environment(args, reservation, REPOSITORY, {}, {"pid": 9001, "starttime": 10})
        reservation.ledger = {"runs": [row]}
        args.command = command[:-1] + ["0" * 64]
        with self.assertRaises(ValueError): admission.prepare_environment(args, reservation, REPOSITORY, {}, {"pid": 9001, "starttime": 10})

    def controls(self):
        leader = self.stage / "leader"
        leader.mkdir(exist_ok=True)
        role = self.stage / "builder"
        (role / "cache").mkdir(parents=True, exist_ok=True)
        row = self.frozen_row()
        (role / "ledger.json").write_text(json.dumps(dict(role="builder", runs=[row])))
        failure = leader / "r015-check-admission-failure-001.json"
        failure.write_bytes((REPOSITORY / ".cache/v0.5.1-s4/leader/r015-check-admission-failure-001.json").read_bytes())
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        config = dict(revision=1, control_debt_seconds=60, old_failure_path=str(failure), old_failure_sha256=sha(failure),
                      common_package_sha256=row["r015_package_sha256"], build_receipt_sha256=row["r015_build_receipt_sha256"],
                      attempt_path=str(leader / "r016-attempt002.json"), used_path=str(role / "cache/r016-attempt002-used.json"))
        debt = leader / "r016-control-debt-002.json"
        debt.write_text(json.dumps(dict(schema="r016-control-debt-v1", role="builder", debt_seconds=60,
                                      meaning="conservative_limit_not_measured", old_failure_path=str(failure),
                                      old_failure_sha256=sha(failure), build_record_sha256=admission.canonical_sha(row))))
        config.update(control_debt_path=str(debt), control_debt_sha256=sha(debt))
        marker = Path(config["attempt_path"])
        marker.write_text(json.dumps(dict(schema="r016-recovery-attempt-v1", role="builder", run_id="run-r015-check-001",
            attempt="attempt002", state="authorized_for_single_claim", old_failure_sha256=sha(failure),
            package_sha256=row["r015_package_sha256"], build_receipt_sha256=row["r015_build_receipt_sha256"],
            build_record_sha256=admission.canonical_sha(row), command_sha256="c" * 64, inner_argv_sha256="d" * 64)))
        context = dict(attempt_sha256=sha(marker), command_sha256="c" * 64, inner_argv_sha256="d" * 64)
        return {"r016": config}, context

    def test_debt_is_bound_and_not_measured_or_double_charged(self):
        authorization, context = self.controls()
        self.assertEqual(admission.control_debt(self.stage, "builder", authorization), 60)
        self.assertEqual(admission.control_debt(self.stage, "builder", authorization), 60)
        self.assertEqual(admission.control_debt(self.stage, "reviewer", authorization), 0)
        damaged = dict(authorization["r016"], control_debt_sha256="0" * 64)
        with self.assertRaises(ValueError): admission.control_debt(self.stage, "builder", {"r016": damaged})

    def test_recovery_claim_once_and_failed_validation_stays_consumed(self):
        authorization, context = self.controls()
        admission.consume_recovery(self.stage, "builder", "run-r015-check-001", authorization, None, context)
        self.assertTrue(Path(authorization["r016"]["used_path"]).is_file())
        with self.assertRaises(FileExistsError): admission.consume_recovery(self.stage, "builder", "run-r015-check-001", authorization, None, context)

    def test_wrong_command_context_claims_before_rejection(self):
        authorization, context = self.controls()
        context["command_sha256"] = "0" * 64
        with self.assertRaises(ValueError): admission.consume_recovery(self.stage, "builder", "run-r015-check-001", authorization, None, context)
        self.assertTrue(Path(authorization["r016"]["used_path"]).is_file())

    def test_early_argv_failure_claims_before_callback(self):
        authorization, context = self.controls()
        def rejected(): raise ValueError("injected argv failure")
        with self.assertRaises(ValueError): admission.consume_recovery(self.stage, "builder", "run-r015-check-001", authorization, None, rejected)
        self.assertTrue(Path(authorization["r016"]["used_path"]).is_file())

    def test_exact_build_and_full_plan(self):
        self.assertTrue(self.check())
        with self.assertRaises(ValueError): self.check(seconds=179)
        with self.assertRaises(ValueError): self.check(run="run-r015-build-002")
        with self.assertRaises(ValueError): self.check(total=2*1024**3-100)

    def test_prerequisite_and_no_retry(self):
        with self.assertRaises(ValueError): self.check(run="run-r015-check-001", seconds=60)
        build = self.valid("run-r015-build-001")
        self.assertTrue(self.check(run="run-r015-check-001", seconds=60, rows=[build]))
        with self.assertRaises(ValueError): self.check(rows=[build])
        build["status"] = "invalid"
        with self.assertRaises(ValueError): self.check(run="run-r015-check-001", seconds=60, rows=[build])

    def test_smoke_is_explicit_not_reset_old_count(self):
        old = {"run_id": "run-smoke-001", "status": "invalid", "kind": "smoke"}
        rows = [old, self.valid("run-r015-build-001"), self.valid("run-r015-check-001")]
        self.assertTrue(self.check(run="run-r015-smoke-001", kind="smoke", seconds=20, rows=rows))
        with self.assertRaises(ValueError): self.check(run="run-r015-smoke-001", kind="selfcheck", seconds=20, rows=rows)
        with self.assertRaises(ValueError): self.check(run="run-r015-smoke-001", kind="smoke", seconds=21, rows=rows)
        self.assertEqual(rows[0], old)

    def test_only_builder_exact_baseline(self):
        rows = [self.valid(name) for name in ("run-r015-build-001", "run-r015-check-001", "run-r015-smoke-001")]
        self.assertTrue(self.check(run="run-r015-baseline-001", kind="baseline", seconds=45, rows=rows))
        with self.assertRaises(ValueError): self.check(run="run-r015-baseline-001", kind="baseline", seconds=45,
                                                     rows=rows, role="reviewer", total=117361673)
        with self.assertRaises(ValueError): self.check(run="run-any-baseline", kind="baseline", seconds=45)

    def test_accounting_unknown_and_budget_drift(self):
        build = self.valid("run-r015-build-001")
        for key, value in (("accounting_errors", ["unknown"]), ("byte_classification_status", "unknown"), ("status", "running")):
            damaged = dict(build, **{key:value})
            with self.assertRaises(ValueError): self.check(run="run-r015-check-001", seconds=60, rows=[damaged])
        self.authorization["r015"]["build_output_bytes"] -= 1
        with self.assertRaises(ValueError): self.check()

if __name__ == "__main__":
    unittest.main()
