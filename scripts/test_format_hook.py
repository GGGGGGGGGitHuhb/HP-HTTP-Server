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

    def test_leaves_unstaged_tracked_untracked_and_vendor_untouched(self):
        untracked = self.root / 'untracked.cpp'
        untracked.write_text('int other(){return 0;}\n')
        vendor = self.root / 'vendor/v.cpp'
        vendor.parent.mkdir()
        vendor.write_bytes(untracked.read_bytes())
        self.run_git('add', 'vendor')
        self.file.write_text('int value(){return 4;}\n')
        self.assertEqual(self.commit().returncode, 0)
        self.assertEqual(untracked.read_bytes(), vendor.read_bytes())
        self.assertEqual(b'int value(){return 4;}\n', self.file.read_bytes())

    def test_unstaged_config_blocks_before_writing(self):
        config = self.root / '.clang-format'
        config.write_text(config.read_text() + '\n# local change\n')
        self.file.write_text('int value(){return 2;}\n')
        self.run_git('add', self.file.name)
        before = self.file.read_bytes()
        index = self.run_git('write-tree')
        self.assertNotEqual(self.commit().returncode, 0)
        self.assertEqual(index, self.run_git('write-tree'))
        self.assertEqual(before, self.file.read_bytes())

    def test_wrong_version_blocks(self):
        self.file.write_text('int value(){return 9;}\n')
        self.run_git('add', self.file.name)
        before = self.file.read_bytes()
        index = self.run_git('write-tree')
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
        self.assertEqual(before, self.file.read_bytes())
        self.assertEqual(index, self.run_git('write-tree'))

    def test_hook_nul_safe_newline_filename(self):
        new = self.root / 'line\nbreak.cpp'
        new.write_text('int fresh(){return 2;}\n')
        self.run_git('add', new.name)
        index = self.run_git('write-tree')
        self.assertNotEqual(self.commit().returncode, 0)
        self.assertEqual(index, self.run_git('write-tree'))
        self.assertEqual(new.read_bytes(), b'int fresh() { return 2; }\n')
        self.run_git('add', new.name)
        self.assertEqual(self.commit().returncode, 0)

    def test_staged_deletion_passes(self):
        self.run_git('rm', self.file.name)
        self.assertEqual(self.commit().returncode, 0)

    def format_scope(self, paths, *args):
        manifest = self.root / 'scope.txt'
        manifest.write_text('\n'.join(paths), encoding='utf-8')
        return subprocess.run(['python3', 'scripts/format_cpp.py', '--files-from',
                               str(manifest), *args], cwd=self.root, capture_output=True)

    def test_scope_new_space_duplicate_and_check(self):
        new = self.root / 'new source.cpp'
        new.write_text('int other(){return 7;}\n')
        before = new.read_bytes()
        index = self.run_git('write-tree')
        self.assertNotEqual(self.format_scope([new.name], '--check').returncode, 0)
        self.assertEqual(before, new.read_bytes())
        result = self.format_scope([new.name, new.name, ''])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b'1 files; 1 changed', result.stdout)
        self.assertEqual(new.read_bytes(), b'int other() { return 7; }\n')
        self.assertEqual(self.format_scope([new.name], '--check').returncode, 0)
        self.assertEqual(index, self.run_git('write-tree'))

    def test_empty_scope_and_hook_conflict(self):
        self.file.write_text('int value(){return 9;}\n')
        before = self.file.read_bytes()
        self.assertEqual(self.format_scope([]).returncode, 0)
        self.assertNotEqual(self.format_scope([], '--hook').returncode, 0)
        self.assertEqual(before, self.file.read_bytes())

    def test_scope_rejections_are_atomic(self):
        self.file.write_text('int value(){return 9;}\n')
        (self.root / 'link.cpp').symlink_to(self.file)
        (self.root / 'linked').symlink_to(self.root, target_is_directory=True)
        for directory in ('vendor', 'generated', 'build', 'build-local', '.cache'):
            (self.root / directory).mkdir()
            (self.root / directory / 'x.cpp').write_text('int x;\n')
        invalid = [str(self.file), '../escape.cpp', 'missing.cpp', 'scope.txt',
                   'link.cpp', 'linked/name with spaces.cpp',
                   *[d + '/x.cpp' for d in ('vendor', 'generated', 'build',
                                           'build-local', '.cache')]]
        before = self.file.read_bytes()
        index = self.run_git('write-tree')
        for path in invalid:
            with self.subTest(path=path):
                self.assertNotEqual(self.format_scope([self.file.name, path]).returncode, 0)
                self.assertEqual(before, self.file.read_bytes())
                self.assertEqual(index, self.run_git('write-tree'))

    def test_rename_and_staged_config_are_scoped(self):
        self.run_git('mv', self.file.name, 'renamed source.cpp')
        renamed = self.root / 'renamed source.cpp'
        renamed.write_text('int value(){return 2;}\n')
        self.run_git('add', renamed.name)
        config = self.root / '.clang-format'
        config.write_text(config.read_text() + '\n# staged change\n')
        self.run_git('add', '.clang-format')
        sentinel = self.root / 'outside.cpp'
        sentinel.write_text('int untouched(){return 8;}\n')
        index = self.run_git('write-tree')
        self.assertNotEqual(self.commit().returncode, 0)
        self.assertEqual(index, self.run_git('write-tree'))
        self.assertEqual(sentinel.read_bytes(), b'int untouched(){return 8;}\n')
        self.run_git('add', renamed.name)
        self.assertEqual(self.commit().returncode, 0)

    def test_selected_symlink_blocks_all_writes(self):
        self.file.write_text('int value(){return 9;}\n')
        (self.root / 'z-link.cpp').symlink_to(self.file.name)
        self.run_git('add', self.file.name, 'z-link.cpp')
        before = self.file.read_bytes()
        index = self.run_git('write-tree')
        self.assertNotEqual(self.commit().returncode, 0)
        self.assertEqual(before, self.file.read_bytes())
        self.assertEqual(index, self.run_git('write-tree'))

    def test_conflict_blocks_all_writes(self):
        blob = self.run_git('rev-parse', 'HEAD:' + self.file.name).strip().decode()
        self.run_git('update-index', '--force-remove', self.file.name)
        entries = ''.join(f'100644 {blob} {stage}\t{self.file.name}\n' for stage in (1, 2, 3))
        subprocess.run(['git', 'update-index', '--index-info'], input=entries.encode(),
                       cwd=self.root, check=True)
        before = self.file.read_bytes()
        index = self.run_git('ls-files', '--stage', '-z')
        result = subprocess.run(['python3', 'scripts/format_cpp.py', '--hook'],
                                cwd=self.root, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('合并冲突'.encode(), result.stderr)
        self.assertEqual(before, self.file.read_bytes())
        self.assertEqual(index, self.run_git('ls-files', '--stage', '-z'))


if __name__ == '__main__':
    unittest.main()
