"""Exercise the real commit hook in disposable repositories."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1]


class HookTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for name in ('.clang-format', '.githooks/pre-commit', 'scripts/format_cpp.py'):
            destination = self.root / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(SOURCE / name, destination)
        self.run_git('init', '-q')
        self.run_git('config', 'user.name', 'Hook Test')
        self.run_git('config', 'user.email', 'hook@example.invalid')
        self.run_git('config', 'core.hooksPath', '.githooks')
        self.run_git('config', 'commit.gpgsign', 'false')
        self.file = self.root / 'name with spaces.cpp'
        self.file.write_text('int value() { return 1; }\n')
        self.run_git('add', '.')
        self.assertEqual(self.commit().returncode, 0)

    def run_git(self, *args):
        return subprocess.check_output(['git', *args], cwd=self.root, stderr=subprocess.PIPE)

    def commit(self):
        return subprocess.run(['git', 'commit', '-qm', 'test', '--allow-empty'],
                              cwd=self.root, capture_output=True)

    def test_format_abort_then_stage_and_pass(self):
        self.file.write_text('int value(){return 2;}\n')
        self.run_git('add', '.')
        index = self.run_git('write-tree')
        head = self.run_git('rev-parse', 'HEAD')
        self.assertNotEqual(self.commit().returncode, 0)
        self.assertEqual(index, self.run_git('write-tree'))
        self.assertEqual(head, self.run_git('rev-parse', 'HEAD'))
        self.assertIn(b'return 2;', self.file.read_bytes())
        self.assertNotEqual(self.commit().returncode, 0)  # old index still rejected
        self.run_git('add', '.')
        self.assertEqual(self.commit().returncode, 0)

    def test_partial_staging_is_untouched(self):
        self.file.write_text('int value(){return 2;}\n')
        self.run_git('add', '.')
        self.file.write_text('int value(){return 3;}\n')
        before = self.file.read_bytes()
        index = self.run_git('write-tree')
        self.assertNotEqual(self.commit().returncode, 0)
        self.assertEqual(before, self.file.read_bytes())
        self.assertEqual(index, self.run_git('write-tree'))

    def test_formats_unstaged_tracked_file_but_not_untracked_or_vendor(self):
        untracked = self.root / 'untracked.cpp'
        untracked.write_text('int other(){return 0;}\n')
        vendor = self.root / 'vendor/v.cpp'
        vendor.parent.mkdir()
        vendor.write_bytes(untracked.read_bytes())
        self.run_git('add', 'vendor')
        self.file.write_text('int value(){return 4;}\n')
        self.assertNotEqual(self.commit().returncode, 0)
        self.assertEqual(untracked.read_bytes(), vendor.read_bytes())
        self.assertEqual(b'int value() { return 4; }\n', self.file.read_bytes())

    def test_unstaged_config_blocks_before_writing(self):
        config = self.root / '.clang-format'
        config.write_text(config.read_text() + '\n# local change\n')
        self.file.write_text('int value(){return 2;}\n')
        before = self.file.read_bytes()
        self.assertNotEqual(self.commit().returncode, 0)
        self.assertEqual(before, self.file.read_bytes())

    def test_wrong_version_blocks(self):
        fake = self.root / 'bin'
        fake.mkdir()
        executable = fake / 'clang-format-18'
        executable.write_text('#!/bin/sh\necho "clang-format version 17.0.0"\n')
        executable.chmod(0o755)
        result = subprocess.run(['git', 'commit', '-qm', 'test', '--allow-empty'],
                                cwd=self.root, capture_output=True,
                                env=dict(os.environ, PATH=str(fake) + ':' + os.environ['PATH']))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'18.1.3', result.stderr)

    def test_staged_deletion_passes(self):
        self.run_git('rm', self.file.name)
        self.assertEqual(self.commit().returncode, 0)


if __name__ == '__main__':
    unittest.main()
