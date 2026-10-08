"""R005 deterministic deletion/exchange injection; no subprocess, socket or sleeps."""
import errno
import json
import os
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch
from budget_v5 import BudgetError, Reservation, file_bytes
from static_inventory_v4 import seal

class ScanTests(unittest.TestCase):
    def test_regular_disappears_before_snapshot(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'gone').write_text('gone')
            removed = []
            original = os.stat
            def inject(name, *args, **kwargs):
                if name == 'gone' and 'dir_fd' in kwargs:
                    os.unlink(name, dir_fd=kwargs['dir_fd'])
                return original(name, *args, **kwargs)
            with patch('budget_v5.os.stat', side_effect=inject):
                self.assertEqual(file_bytes(root, disappearance=removed.append), 0)
            self.assertEqual(removed, ['gone'])

    def test_directory_disappears_before_open(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'gone').mkdir()
            original = os.open
            removed = []
            def inject(name, *args, **kwargs):
                if name == 'gone' and 'dir_fd' in kwargs:
                    os.rmdir(name, dir_fd=kwargs['dir_fd'])
                return original(name, *args, **kwargs)
            with patch('budget_v5.os.open', side_effect=inject):
                self.assertEqual(file_bytes(root, disappearance=removed.append), 0)
            self.assertEqual(removed, ['gone'])

    def fixture(self, root):
        case = root / 'integration-Abc123'
        (case / 'root').mkdir(parents=True)
        (case / 'sibling-secret.txt').write_text('secret')
        link = case / 'root/escape.txt'
        link.symlink_to(case / 'sibling-secret.txt')
        return link

    def test_link_disappears_at_readlink(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root)
            original = os.readlink
            removed = []
            def inject(name, *args, **kwargs):
                if name == 'escape.txt' and 'dir_fd' in kwargs:
                    os.unlink(name, dir_fd=kwargs['dir_fd'])
                return original(name, *args, **kwargs)
            with patch('budget_v5.os.readlink', side_effect=inject):
                self.assertEqual(file_bytes(root, root, removed.append), 6)
            self.assertEqual(removed, ['integration-Abc123/root/escape.txt'])

    def test_one_snapshot_per_regular_or_link(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            link = self.fixture(root)
            link_bytes = link.lstat().st_size
            original = os.stat
            calls = {}
            def observe(name, *args, **kwargs):
                if 'dir_fd' in kwargs:
                    calls[name] = calls.get(name, 0) + 1
                return original(name, *args, **kwargs)
            with patch('budget_v5.os.stat', side_effect=observe):
                self.assertEqual(file_bytes(root, root), 6 + link_bytes)
            self.assertEqual(calls['sibling-secret.txt'], 1)
            self.assertEqual(calls['escape.txt'], 1)

    def test_permission_error_is_not_disappearance(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'denied').write_text('x')
            original = os.stat
            def inject(name, *args, **kwargs):
                if name == 'denied' and 'dir_fd' in kwargs:
                    raise PermissionError(errno.EACCES, 'denied')
                return original(name, *args, **kwargs)
            with patch('budget_v5.os.stat', side_effect=inject), self.assertRaises(PermissionError):
                file_bytes(root)

    def test_directory_swapped_for_link_never_followed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'output'
            root.mkdir()
            (root / 'swap').mkdir()
            outside = Path(directory) / 'outside'
            outside.mkdir()
            (outside / 'secret').write_text('never read')
            original = os.open
            def inject(name, *args, **kwargs):
                if name == 'swap' and 'dir_fd' in kwargs:
                    os.rmdir(name, dir_fd=kwargs['dir_fd'])
                    os.symlink(outside, name, dir_fd=kwargs['dir_fd'])
                return original(name, *args, **kwargs)
            with patch('budget_v5.os.open', side_effect=inject), self.assertRaises(OSError) as raised:
                file_bytes(root)
            self.assertIn(raised.exception.errno, (errno.ENOTDIR, errno.ELOOP))

    def test_unknown_and_raw_links_still_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'target').write_text('x')
            (root / 'unknown').symlink_to(root / 'target')
            for fixture in (None, root):
                with self.assertRaises(BudgetError):
                    file_bytes(root, fixture)

    def test_static_disappearance_remains_invalid(self):
        with tempfile.TemporaryDirectory() as directory:
            role = Path(directory) / 'builder'
            source = role / 'E-S3/source'
            source.mkdir(parents=True)
            (source / 'original').write_text('original')
            seal(role)
            value = Reservation(role.parent, 'builder', 'run-synthetic', 'selfcheck', 20)
            value.static_inventory = json.loads((role / 'static-inventory-v4.json').read_text())['files']
            (source / 'original').unlink()
            with self.assertRaises(BudgetError):
                value.output_bytes()

if __name__ == '__main__':
    unittest.main()
