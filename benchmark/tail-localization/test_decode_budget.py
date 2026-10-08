import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from budget import BudgetError, Reservation
from decode import DecodeError, HEADER, MAGIC, RECORD, records


class DecodeTests(unittest.TestCase):
    def fixture(self, directory, flags=1, kind=1, trailing=b""):
        data = HEADER.pack(MAGIC, HEADER.size, RECORD.size, flags, 1, 10, 10, 11, 100, 50, 50)
        data += RECORD.pack(50, 0, 1, 1, kind, 0, 0) + trailing
        path = Path(directory) / "events.bin"
        path.write_bytes(data)
        return path, dict(pid=10, tid=11, starttime=100, endpoint="client", connection_ids=[1],
                          recording_start_ns=0, recording_end_ns=100, sha256=hashlib.sha256(data).hexdigest())

    def test_valid_layout(self):
        with tempfile.TemporaryDirectory() as directory:
            path, metadata = self.fixture(directory)
            self.assertEqual(len(list(records(path, metadata))), 1)

    def test_overflow_unknown_and_trailer_fail(self):
        for arguments in (dict(flags=3), dict(kind=100), dict(trailing=b"x")):
            with tempfile.TemporaryDirectory() as directory:
                path, metadata = self.fixture(directory, **arguments)
                with self.assertRaises(DecodeError):
                    list(records(path, metadata))

    def test_hash_and_owner_fail(self):
        for field, value in (("sha256", "0" * 64), ("tid", 12)):
            with tempfile.TemporaryDirectory() as directory:
                path, metadata = self.fixture(directory)
                metadata[field] = value
                with self.assertRaises(DecodeError):
                    list(records(path, metadata))


class BudgetTests(unittest.TestCase):
    def stage(self, directory):
        root = Path(directory) / "stage"
        (root / "leader").mkdir(parents=True)
        (root / "builder").mkdir()
        (root / "reviewer").mkdir()
        auth = dict(roles_serial=True, per_role_dynamic_seconds=1200, per_role_output_bytes=2 * 1024 * 1024,
                    builder_http_limits=dict(smoke=2, staircase=8, observed_abba=4),
                    reviewer_http_limits=dict(smoke=2, confirmation_ab=2), old_s3_ledgers=dict(builder={}, reviewer={}))
        (root / "leader/authorization.json").write_text(json.dumps(auth))
        return root

    def test_valid_and_failed_charge(self):
        with tempfile.TemporaryDirectory() as directory:
            root = self.stage(directory)
            with Reservation(root, "builder", "run-ok", "selfcheck", 10):
                pass
            with self.assertRaises(RuntimeError):
                with Reservation(root, "builder", "run-fail", "selfcheck", 10):
                    raise RuntimeError("synthetic failure")
            runs = json.loads((root / "builder/ledger.json").read_text())["runs"]
            self.assertEqual([run["status"] for run in runs], ["valid", "invalid"])
            self.assertTrue(all(run["charged_seconds"] >= 0 for run in runs))

    def test_bad_path_kind_and_unsettled_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            root = self.stage(directory)
            for run_id, kind in (("run-../escape", "selfcheck"), ("run-okay", "newload")):
                with self.assertRaises(BudgetError):
                    with Reservation(root, "builder", run_id, kind, 10):
                        pass
            (root / "reviewer/ledger.json").write_text(json.dumps(dict(runs=[dict(status="running")])))
            with self.assertRaises(BudgetError):
                with Reservation(root, "builder", "run-ok", "selfcheck", 10):
                    pass

    def test_unknown_file_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = self.stage(directory)
            (root / "builder/unclassified.bin").write_bytes(b"x")
            with self.assertRaises(BudgetError):
                with Reservation(root, "builder", "run-unknown", "selfcheck", 10):
                    pass


if __name__ == "__main__":
    unittest.main()
