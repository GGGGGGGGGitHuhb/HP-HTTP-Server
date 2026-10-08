"""R003 synthetic path/accounting regressions; no socket or subprocess."""
import hashlib
import tempfile
from pathlib import Path
import unittest
from budget_v4 import BudgetError, Reservation
from static_inventory_v4 import seal
import json

class RouteTests(unittest.TestCase):
    def setup_role(self, directory):
        role = Path(directory) / "builder"
        (role / "E-S3/source").mkdir(parents=True)
        (role / "E-S3/source/original.py").write_text("original")
        return role

    def reservation(self, role):
        value = Reservation(role.parent, "builder", "run-synthetic", "selfcheck", 20)
        value.static_inventory = json.loads((role / "static-inventory-v4.json").read_text())["files"]
        return value

    def test_source_tmp_inside_repository_and_counted(self):
        with tempfile.TemporaryDirectory() as directory:
            role = self.setup_role(directory)
            root = role / "E-S3/source/.test-tmp"
            root.mkdir()
            (root / "output.json").write_bytes(b"12345")
            seal(role)
            value = self.reservation(role)
            self.assertTrue(root.resolve().is_relative_to((role / "E-S3/source").resolve()))
            self.assertNotIn("E-S3/source/.test-tmp/output.json", value.static_inventory)
            self.assertEqual(value.output_bytes(), 5 + (role / "static-inventory-v4.json").stat().st_size)
            self.assertEqual(value.dynamic_bytes(), value.output_bytes())

    def test_old_testing_and_tmp_still_counted(self):
        with tempfile.TemporaryDirectory() as directory:
            role = self.setup_role(directory)
            for name in ("build-E/Testing", "build-E/test-tmp"):
                root = role / name
                root.mkdir(parents=True)
                (root / "data").write_bytes(b"abc")
            seal(role)
            value = self.reservation(role)
            self.assertEqual(value.output_bytes(), 6 + (role / "static-inventory-v4.json").stat().st_size)

    def test_source_unknown_file_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            role = self.setup_role(directory)
            seal(role)
            (role / "E-S3/source/unregistered").write_text("unknown")
            with self.assertRaises(BudgetError):
                self.reservation(role).output_bytes()

    def test_original_source_hash_protected(self):
        with tempfile.TemporaryDirectory() as directory:
            role = self.setup_role(directory)
            seal(role)
            value = self.reservation(role)
            value.static_inventory_sha256 = hashlib.sha256((role / "static-inventory-v4.json").read_bytes()).hexdigest()
            value.verify_static_hashes()
            (role / "E-S3/source/original.py").write_text("modified")
            with self.assertRaises(BudgetError):
                value.verify_static_hashes()

    def test_new_fixture_links_nofollow(self):
        with tempfile.TemporaryDirectory() as directory:
            role = self.setup_role(directory)
            root = role / "E-S3/source/.test-tmp"
            case = root / "static-Abc123"
            (case / "root").mkdir(parents=True)
            target = case / "sibling-secret.txt"
            target.write_bytes(b"secret")
            link = case / "root/escape.txt"
            link.symlink_to(target)
            seal(role)
            self.assertEqual(self.reservation(role).output_bytes(), 6 + link.lstat().st_size + (role / "static-inventory-v4.json").stat().st_size)
            link.unlink()
            link.symlink_to(role / "E-S3/source/original.py")
            with self.assertRaises(BudgetError):
                self.reservation(role).output_bytes()

if __name__ == "__main__":
    unittest.main()
