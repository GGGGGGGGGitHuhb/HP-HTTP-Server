#!/usr/bin/env python3
"""Charged, small non-network contracts for the fixed R028 entry."""
import argparse
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tarfile
import time
import unittest

sys.dont_write_bytecode = True
HERE = Path(__file__).absolute().parent
spec = importlib.util.spec_from_file_location('r028_candidate', HERE / 'revalidate.py')
candidate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(candidate)


class Contracts(unittest.TestCase):
    def setUp(self):
        self.root = OUTPUT / self._testMethodName
        self.root.mkdir()
        self.previous_root = candidate.ROOT
        candidate.ROOT = self.root
        for role in ('builder', 'reviewer'):
            (self.root / role / 'control').mkdir(parents=True)
        self.scope = candidate.Scope(self.root / 'builder', 10, 15)
        self.scope.poll()

    def tearDown(self):
        try:
            self.scope.finish()
        finally:
            candidate.ROOT = self.previous_root

    def test_exact_role_parent_and_missing_leaf(self):
        parent = candidate.role_root('builder')
        target = parent / 'smoke-C-001'
        self.assertFalse(target.exists())
        candidate.create_leaf(target, parent)
        self.assertTrue(target.is_dir())
        with self.assertRaisesRegex(FileExistsError, ''):
            candidate.create_leaf(target, parent)
        with self.assertRaisesRegex(ValueError, 'exact declared parent'):
            candidate.create_leaf(self.root / 'reviewer/smoke-C-001', parent)
        with self.assertRaisesRegex(ValueError, 'role rejected'):
            candidate.role_root('other')

    def test_wrong_commit_tree_rejected(self):
        for key in ('commit', 'tree'):
            data = dict(label='C', commit=candidate.IDENTITIES['C'][0], tree=candidate.IDENTITIES['C'][1])
            data[key] = 'wrong'
            with self.assertRaisesRegex(ValueError, 'fixed commit/tree mismatch'):
                candidate.validate_manifest(data, 'builder', 'C', self.scope)

    def test_real_entry_repeat_result_preserved(self):
        path = self.root / 'builder/control/build-C.result.json'
        path.write_bytes(b'original failure evidence\n')
        before = candidate.digest(path)
        with self.assertRaisesRegex(ValueError, 'step already attempted'):
            candidate.main(['--role', 'builder', 'build', '--label', 'C'])
        self.assertEqual(candidate.digest(path), before)

    def test_wrong_binary_rejected(self):
        output = self.root / 'builder/C'
        (output / 'source').mkdir(parents=True)
        (output / 'build').mkdir()
        content = b'fixed source\n'
        (output / 'source/main.cpp').write_bytes(content)
        with tarfile.open(output / 'source.tar', 'w') as archive:
            entry = tarfile.TarInfo('main.cpp'); entry.size = len(content)
            archive.addfile(entry, io.BytesIO(content))
        data = {'label': 'C', 'commit': candidate.IDENTITIES['C'][0], 'tree': candidate.IDENTITIES['C'][1],
                'source': str(output / 'source'), 'archive': str(output / 'source.tar'),
                'archive_sha256': candidate.digest(output / 'source.tar'),
                'source_hashes': {'main.cpp': hashlib.sha256(content).hexdigest()}, 'flags': candidate.FLAGS,
                'binary': str(output / 'build/hp_http_server'), 'binary_sha256': 'wrong'}
        (output / 'build/hp_http_server').write_bytes(b'not the expected binary')
        # Binary is checked before runtime commands; the real hash comparison rejects it.
        with self.assertRaisesRegex(ValueError, 'binary changed'):
            candidate.validate_manifest(data, 'builder', 'C', self.scope)

    def test_regular_tree_accounting(self):
        (self.root / 'builder/data.bin').write_bytes(b'abc')
        (self.root / 'builder/process.stderr').write_bytes(b'12345')
        result = candidate.measure(self.root / 'builder')
        self.assertEqual(result['logical_bytes'], 8)
        self.assertEqual(result['log_logical_bytes'], 5)
        self.assertGreaterEqual(result['allocated_bytes'], 8)

    def test_archive_rejects_escape(self):
        archive = self.root / 'escape.tar'
        with tarfile.open(archive, 'w') as stream:
            entry = tarfile.TarInfo('../escape'); entry.size = 1
            stream.addfile(entry, io.BytesIO(b'x'))
        with self.assertRaisesRegex(ValueError, 'unsafe archive path'):
            candidate.extract_archive(archive, self.root / 'source')
        self.assertFalse((self.root / 'escape').exists())

    def test_real_child_failure_reaped(self):
        with self.assertRaisesRegex(ValueError, 'command failed'):
            self.scope.command(['/usr/bin/python3', '-I', '-B', '-c', 'raise SystemExit(3)'])
        self.assertEqual(self.scope.cleanup[-1]['returncode'], 3)
        self.assertTrue(self.scope.cleanup[-1]['reaped'])
        self.assertFalse(self.scope.cleanup[-1]['errors'])

    def test_real_deadline_child_cleanup(self):
        self.scope.work_deadline = time.monotonic() + .15
        with self.assertRaisesRegex(ValueError, 'internal work deadline'):
            self.scope.command(['/usr/bin/python3', '-I', '-B', '-c', 'import time; time.sleep(10)'])
        self.assertTrue(self.scope.cleanup[-1]['reaped'])
        self.assertFalse(self.scope.cleanup[-1]['errors'])
        self.assertEqual(self.scope.cleanup[-1]['identity']['pgid'], os.getpgrp())

    def test_real_entry_invalid_role(self):
        process = subprocess.run(['/usr/bin/python3', '-B', str(HERE / 'revalidate.py'), '--role', 'other', 'build', '--label', 'C'],
                                 capture_output=True, timeout=2)
        self.assertEqual(process.returncode, 2)
        self.assertIn(b'invalid choice', process.stderr)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True)
    parser.add_argument('--shared-work-deadline-ns', type=int)
    parser.add_argument('--shared-cleanup-deadline-ns', type=int)
    args = parser.parse_args()
    global OUTPUT
    OUTPUT = Path(args.output).absolute()
    if args.shared_work_deadline_ns is not None or args.shared_cleanup_deadline_ns is not None:
        candidate.require(args.shared_work_deadline_ns is not None and args.shared_cleanup_deadline_ns is not None,
                          'both shared contract deadlines required')
        candidate.SHARED_WORK_DEADLINE = args.shared_work_deadline_ns / 1e9
        candidate.SHARED_CLEANUP_DEADLINE = args.shared_cleanup_deadline_ns / 1e9 - 2
        candidate.require(time.monotonic() < candidate.SHARED_WORK_DEADLINE < candidate.SHARED_CLEANUP_DEADLINE,
                          'shared contract deadline exhausted/invalid')
    stage = HERE.parents[1] / '.cache/v0.5.1-revalidation'
    roles = [stage / role for role in ('builder', 'reviewer')]
    candidate.require(OUTPUT in [role / 'contract-001' for role in roles], 'exact contract output required')
    candidate.create_leaf(OUTPUT, OUTPUT.parent)
    previous = signal.getsignal(signal.SIGTERM)
    def terminate(signum, frame):
        raise TimeoutError('contract TERM')
    signal.signal(signal.SIGTERM, terminate)
    result = None
    error = None
    started = time.monotonic()
    try:
        candidate.preflight(OUTPUT.parent)
        candidate.measure(OUTPUT.parent)
        for path in (HERE / 'revalidate.py', HERE / 'test_revalidate.py'):
            compile(path.read_bytes(), str(path), 'exec')
        result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(Contracts))
        candidate.measure(OUTPUT.parent)
    except BaseException as failure:
        error = {'type': type(failure).__name__, 'message': str(failure)}
    finally:
        try:
            signal.signal(signal.SIGTERM, signal.SIG_IGN)
        except BaseException as failure:
            error = error or {'type': type(failure).__name__}
        try:
            candidate.save(OUTPUT / 'contract-result.json', {'status': 'valid' if result and result.wasSuccessful() and error is None else 'invalid',
             'tests': result.testsRun if result else None, 'failures': len(result.failures) if result else None,
             'errors': len(result.errors) if result else None, 'error': error,
             'shared_work_deadline_ns': args.shared_work_deadline_ns, 'shared_cleanup_deadline_ns': args.shared_cleanup_deadline_ns,
             'internal_elapsed_seconds': time.monotonic() - started, 'external_wall_required': True})
        except BaseException as failure:
            error = error or {'type': type(failure).__name__, 'message': 'contract evidence unavailable'}
            try:
                print('contract evidence unavailable; invalid', file=sys.stderr)
            except BaseException:
                pass
        try:
            signal.signal(signal.SIGTERM, previous)
        except BaseException as failure:
            error = error or {'type': type(failure).__name__}
    candidate.require(result is not None and result.wasSuccessful() and error is None, 'contract failed; stop')


if __name__ == '__main__':
    main()
