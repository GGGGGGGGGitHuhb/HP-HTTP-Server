"""R002 narrow fixture/accounting regressions; no HTTP, no subprocess."""
import json
import os
from pathlib import Path
import tempfile
import unittest
from budget_v3 import BudgetError, Reservation, file_bytes


class FixtureGuardTests(unittest.TestCase):
    def fixture(self, directory, prefix="static"):
        root = Path(directory) / "build-E/test-tmp"
        case = root / (prefix + "-Abc123")
        (case / "root").mkdir(parents=True)
        (case / "sibling-secret.txt").write_bytes(b"secret")
        return root, case

    def test_allowed_file_and_directory_links_count_once(self):
        with tempfile.TemporaryDirectory() as directory:
            root, case = self.fixture(directory)
            links = [case / "root/escape.txt", case / "root/escape-dir", case / "root-link"]
            links[0].symlink_to(case / "sibling-secret.txt")
            links[1].symlink_to(case, target_is_directory=True)
            links[2].symlink_to(case / "root", target_is_directory=True)
            self.assertEqual(file_bytes(root, root), 6 + sum(path.lstat().st_size for path in links))

    def test_original_integration_link_allowed(self):
        with tempfile.TemporaryDirectory() as directory:
            root, case = self.fixture(directory, "integration")
            link = case / "root/escape.txt"
            link.symlink_to(case / "sibling-secret.txt")
            self.assertEqual(file_bytes(root, root), 6 + link.lstat().st_size)

    def test_external_and_unknown_fixture_links_rejected(self):
        for mode in ("external", "unknown"):
            with tempfile.TemporaryDirectory() as directory:
                root, case = self.fixture(directory)
                if mode == "external":
                    target = Path(directory) / "outside.txt"
                    target.write_text("outside")
                    link = case / "root/escape.txt"
                else:
                    target = case / "sibling-secret.txt"
                    link = case / "root/new-link"
                link.symlink_to(target)
                with self.assertRaises(BudgetError):
                    file_bytes(root, root)

    def test_ordinary_output_link_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "target").write_text("ordinary")
            (root / "raw.stdout").symlink_to(root / "target")
            with self.assertRaises(BudgetError):
                file_bytes(root)

    def test_sparse_file_uses_logical_size(self):
        with tempfile.TemporaryDirectory() as directory:
            root, case = self.fixture(directory)
            path = case / "root/oversized.bin"
            with path.open("wb") as stream:
                stream.truncate(8 * 1024 * 1024 + 1)
            self.assertEqual(file_bytes(root, root), 6 + 8 * 1024 * 1024 + 1)

    def test_classification_failure_still_settles_full_reservation(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory) / "stage"
            (stage / "leader").mkdir(parents=True)
            (stage / "builder").mkdir()
            (stage / "reviewer").mkdir()
            auth = dict(roles_serial=True, per_role_dynamic_seconds=1200, per_role_output_bytes=2**31,
                        builder_http_limits={}, reviewer_http_limits={}, old_s3_ledgers=dict(builder={}, reviewer={}))
            (stage / "leader/authorization.json").write_text(json.dumps(auth))
            with self.assertRaises(BudgetError):
                with Reservation(stage, "builder", "run-bad-bytes", "selfcheck", 20) as reservation:
                    target = reservation.output / "target"
                    target.write_text("fixture")
                    (reservation.output / "bad.stdout").symlink_to(target)
            run = json.loads((stage / "builder/ledger.json").read_text())["runs"][0]
            self.assertEqual(run["status"], "invalid")
            self.assertEqual(run["charged_seconds"], 20)
            self.assertIsNone(run["role_output_bytes"])
            self.assertTrue(run["accounting_errors"])
            with self.assertRaisesRegex(BudgetError, "unknown bytes"):
                with Reservation(stage, "builder", "run-next", "selfcheck", 20):
                    pass


if __name__ == "__main__":
    unittest.main()
