"""R015 外治理真实准入函数反例；由具名 check60 计费执行。"""
import importlib.util
from pathlib import Path
import sys
import unittest
sys.dont_write_bytecode = True
MODULE = Path(__file__).absolute().with_name("r015_admission.py")
spec = importlib.util.spec_from_file_location("r015_admission_under_test", MODULE)
admission = importlib.util.module_from_spec(spec)
spec.loader.exec_module(admission)

class AdmissionTests(unittest.TestCase):
    def setUp(self):
        self.authorization = {"per_role_output_bytes": 2 * 1024**3,
            "r015": {"revision": 1, "no_automatic_retries": True,
                     "per_role_output_bytes": 512*1024**2, "build_output_bytes": 320*1024**2,
                     "baseline_output_bytes": 128*1024**2, "other_output_bytes": 64*1024**2}}

    def check(self, run="run-r015-build-001", kind="selfcheck", seconds=180, rows=(), role="builder", total=1486288348):
        return admission.check_slot(role, run, kind, seconds, self.authorization, list(rows), total, 16*1024**2)

    def valid(self, run):
        return {"run_id": run, "status": "valid", "accounting_errors": [], "byte_accounting": "verified"}

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
        for key, value in (("accounting_errors", ["unknown"]), ("byte_accounting", "unknown"), ("status", "running")):
            damaged = dict(build, **{key:value})
            with self.assertRaises(ValueError): self.check(run="run-r015-check-001", seconds=60, rows=[damaged])
        self.authorization["r015"]["build_output_bytes"] -= 1
        with self.assertRaises(ValueError): self.check()

if __name__ == "__main__":
    unittest.main()
