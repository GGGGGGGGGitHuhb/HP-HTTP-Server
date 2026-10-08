#!/usr/bin/env python3
"""Prospective R029 independent driver; no execution authorized by this file."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
DRIVER_STARTED = time.monotonic()
HERE = Path(__file__).absolute().parent
spec = importlib.util.spec_from_file_location('r029_candidate', HERE / 'supervise.py')
candidate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(candidate)


def digest(value):
    return hashlib.sha256(value).hexdigest()


def encoded(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False).encode()


def actual_identity(pid):
    try:
        text = Path('/proc', str(pid), 'stat').read_text()
    except OSError as error:
        if error.errno in (2, 3):
            return None
        raise
    tokens = text[text.rfind(')') + 2:].split()
    return {'pid': pid, 'starttime': int(tokens[19]), 'pgid': int(tokens[2]), 'sid': int(tokens[3]), 'ppid': int(tokens[1])}


def still_owned(record):
    current = actual_identity(record['pid'])
    return current is not None and all(current[key] == record[key] for key in ('pid', 'starttime', 'pgid', 'sid'))


def register_tree(root, registry):
    parents = [root]
    while parents:
        parent = parents.pop()
        try:
            children = Path('/proc', str(parent), 'task', str(parent), 'children').read_text().split()
        except OSError as error:
            if error.errno in (2, 3):
                continue
            raise
        for text in children:
            child = actual_identity(int(text))
            if child is not None and child['ppid'] == parent:
                if (child['pid'], child['starttime']) not in registry:
                    if len(registry) >= 128:
                        raise ValueError('driver descendant bound')
                    registry[(child['pid'], child['starttime'])] = child
                parents.append(child['pid'])


def driver_cleanup(process, registry, deadline):
    """Own supervision is independent of candidate functions and settlement."""
    errors = []
    actions = []
    for signum in (signal.SIGTERM, signal.SIGKILL):
        for record in reversed(list(registry.values())):
            try:
                if still_owned(record):
                    if time.monotonic() >= deadline:
                        raise TimeoutError('independent cleanup deadline')
                    os.kill(record['pid'], signum)
                    actions.append({'identity': record, 'signal': signum})
            except OSError as error:
                if error.errno not in (2, 3):
                    errors.append(str(error))
            except BaseException as error:
                errors.append(str(error))
        if process is not None:
            try:
                process.wait(timeout=min(.3, max(.001, deadline - time.monotonic())))
            except subprocess.TimeoutExpired:
                pass
            except BaseException as error:
                errors.append(str(error))
        alive = False
        for record in registry.values():
            try:
                alive = still_owned(record) or alive
            except BaseException as error:
                errors.append(str(error))
                alive = True
        if not alive:
            break
    remaining = []
    for record in registry.values():
        try:
            if still_owned(record):
                remaining.append(record)
        except BaseException as error:
            remaining.append(dict(record, unknown=str(error)))
    return {'actions': actions, 'errors': errors, 'remaining': remaining}


class PureContracts(unittest.TestCase):
    def test_pid_identity_ignores_reparent_not_reuse(self):
        before = {'pid': 31, 'starttime': 4, 'pgid': 30, 'sid': 1, 'ppid': 30}
        self.assertTrue(candidate.same_object(before, dict(before, ppid=1)))
        self.assertFalse(candidate.same_object(before, dict(before, starttime=5)))

    def test_wall_zero_nonzero_and_missing(self):
        self.assertEqual(candidate.parse_wall('0.30\n'), .30)
        self.assertEqual(candidate.parse_wall('Command exited with non-zero status 3\n0.01\n'), .01)
        self.assertEqual(candidate.parse_wall('Command terminated by signal 9\n2.00\n'), 2)
        for text in ('', 'nan\n', '-1\n', 'unexpected\n0.10\n'):
            with self.assertRaisesRegex(ValueError, 'time'):
                candidate.parse_wall(text)

    def test_same_namespace_scan_failure_remains_unknown(self):
        group = {'pid': 31, 'ppid': 30, 'pgid': 31, 'sid': 1, 'starttime': 4, 'state': 'S'}
        namespace = {'device': 2, 'inode': 3}
        with patch.object(candidate, 'namespace_identity', return_value=namespace), patch.object(candidate, 'identity', side_effect=PermissionError(13, 'denied')):
            observation = candidate.scan_group(group, namespace)
        self.assertFalse(observation['complete'])
        self.assertTrue(observation['unknown'])
        self.assertIn('denied', observation['unknown'][0]['message'])

    def test_row_wrong_hash_and_exact_role_path(self):
        row = TABLE['rows'][0]
        with self.assertRaisesRegex(ValueError, 'row SHA'):
            candidate.check_row(TABLE, ROLE, row['step'], 12, CONTROL, 'wrong')
        with self.assertRaisesRegex(ValueError, 'exact control root'):
            candidate.check_row(TABLE, ROLE, row['step'], 12, CONTROL.parent / 'same-name', digest(encoded(row)))

    def test_complete_argv_not_arbitrary_shell(self):
        row = dict(TABLE['rows'][0], argv=['/bin/sh', '-c', 'echo hidden'])
        data = {'schema': TABLE['schema'], 'rows': [row]}
        with self.assertRaisesRegex(ValueError, 'fixed non-network program'):
            candidate.check_row(data, ROLE, row['step'], 12, CONTROL, digest(encoded(row)))

    def test_existing_output_rejected_without_overwrite(self):
        directory = CONTROL / 'duplicate-small'
        directory.mkdir()
        original = directory / 'proof.json'
        candidate.publish(original, {'old': True})
        before = original.read_bytes()
        with self.assertRaises(FileExistsError):
            candidate.publish(original, {'new': True})
        self.assertEqual(original.read_bytes(), before)


def run_case(row, inputs_path, inputs_sha, deadline, cleanup_deadline=None):
    output = CONTROL / ('r029-' + row['step'] + '-001')
    argv = ['/usr/bin/python3', '-B', str(HERE / 'supervise.py'), '--role', ROLE, '--step', row['step'],
            '--table', str(CONTROL / 'fixture-rows.json'), '--table-sha256', TABLE_SHA,
            '--row-sha256', digest(encoded(row)), '--output-control-root', str(CONTROL),
            '--maximum-seconds', '12', '--inputs', str(inputs_path), '--inputs-sha256', inputs_sha]
    if row['step'] in ('supervisor-publish-stderr-001', 'supervisor-pre-handshake-001'):
        argv = ['/usr/bin/python3', '-B', str(HERE / 'test_supervise.py'), '--r030-inject', row['step'], '--'] + argv[3:]
    registry = {}
    process = None
    signaled = False
    transcript = {'argv': argv, 'case': row['step']}
    streams = []
    try:
        for suffix in ('stdout', 'stderr'):
            streams.append((CONTROL / (row['step'] + '.driver.' + suffix)).open('xb'))
        process = subprocess.Popen(argv, cwd=HERE.parents[1], env=row['environment'], stdout=streams[0], stderr=streams[1])
        initial = actual_identity(process.pid)
        if initial is None:
            raise ValueError('driver child identity absent')
        registry[(initial['pid'], initial['starttime'])] = initial
        while process.poll() is None:
            if time.monotonic() >= deadline - 4:
                raise TimeoutError('independent driver work deadline')
            register_tree(process.pid, registry)
            if row['step'] == 'supervisor-parent-term-001' and not signaled and (output / 'work.stdout').is_file() and 'WORK_STARTED' in (output / 'work.stdout').read_text():
                if not still_owned(initial):
                    raise ValueError('parent TERM target changed')
                os.kill(process.pid, signal.SIGTERM)
                signaled = True
            time.sleep(.02)
        transcript['returncode'] = process.wait(timeout=.1)
        result = json.loads((output / 'settlement.json').read_text())
        transcript['candidate_result'] = result
        if row['step'] == 'supervisor-normal-001':
            if process.returncode != 0 or result['status'] != 'valid' or not result['cleanup_complete']:
                raise ValueError('normal real CLI not valid')
            if result['namespace'] != candidate.namespace_identity():
                raise ValueError('same namespace observation missing')
            if result['complete_elapsed_seconds'] < result['time_wall_seconds'] - .02:
                raise ValueError('complete time cannot precede child wall')
        else:
            if process.returncode == 0 or result['status'] == 'valid':
                raise ValueError('failure scenario incorrectly valid')
            if row['step'] in ('supervisor-nonzero-001', 'supervisor-tree-001') and not result['cleanup_complete']:
                raise ValueError('normal nonzero subtree not cleared')
            if row['step'] == 'supervisor-handshake-reject-001':
                if 'WORK_STARTED' in (output / 'work.stdout').read_text() or 'fixture handshake rejection' not in result['first_error']['message']:
                    raise ValueError('rejected handshake executed work/missed reason')
            if row['step'] == 'supervisor-parent-term-001' and (not signaled or 'received TERM' not in result['first_error']['message']):
                raise ValueError('parent TERM branch missed')
        if row['step'] == 'supervisor-publish-stderr-001':
            if result['status'] != 'unknown' or result['first_error']['message'] != 'injected publish failure':
                raise ValueError('publish/stderr failure did not preserve first cause')
            evidence = json.loads((output / 'injection-evidence.json').read_text())
            if not {'wall', 'space', 'inputs', 'settlement'} <= set(evidence['attempted']):
                raise ValueError('failure report skipped later finally checks')
        if row['step'] == 'supervisor-pre-handshake-001':
            if 'WORK_STARTED' in (output / 'work.stdout').read_text() or result['group'] is not None or not result['pre_group_observations']:
                raise ValueError('pre-handshake target not reached or work released')
            if result['cleanup_complete'] or result['pre_group_remaining']:
                raise ValueError('unknown group promoted or registered timeout remains')
        elif result['final_group_observation'] is None:
            raise ValueError('real post-wait group scan missing')
    finally:
        cleanup = driver_cleanup(process, registry, cleanup_deadline if cleanup_deadline is not None else deadline)
        transcript['independent_cleanup'] = cleanup
        for stream in streams:
            try:
                stream.close()
            except BaseException as error:
                cleanup['errors'].append(str(error))
        candidate.publish(CONTROL / (row['step'] + '.driver-result.json'), transcript)
        if cleanup['remaining'] or cleanup['errors']:
            raise ValueError('independent driver cleanup unknown')
    return transcript


def main():
    if len(sys.argv) > 2 and sys.argv[1] == '--r030-inject':
        scenario = sys.argv[2]
        if sys.argv[3] != '--' or scenario not in ('supervisor-publish-stderr-001', 'supervisor-pre-handshake-001'):
            raise ValueError('invalid fixed injection selection')
        arguments = sys.argv[4:]
        control = Path(arguments[arguments.index('--output-control-root') + 1])
        output = control / ('r029-' + scenario + '-001')
        attempted = []
        original_publish, original_verify, original_measure = candidate.publish, candidate.verify_inputs, candidate.measure_tree
        original_read = Path.read_text
        class FailedStderr:
            def write(self, value):
                raise OSError('injected stderr failure')
            def flush(self):
                pass
        def publishing(path, value):
            if Path(path) == output / 'group-observations.json' and scenario == 'supervisor-publish-stderr-001':
                attempted.clear()
                attempted.append('publish')
                raise OSError('injected publish failure')
            if Path(path) == output / 'settlement.json':
                attempted.append('settlement')
            return original_publish(path, value)
        def measuring(path):
            attempted.append('space')
            return original_measure(path)
        def verifying(*args):
            attempted.append('inputs')
            return original_verify(*args)
        def reading(path, *args, **kwargs):
            if path == output / 'time.wall':
                attempted.append('wall')
            return original_read(path, *args, **kwargs)
        try:
            with patch.object(candidate, 'publish', publishing), patch.object(candidate, 'verify_inputs', verifying), patch.object(candidate, 'measure_tree', measuring), patch.object(Path, 'read_text', reading):
                if scenario == 'supervisor-publish-stderr-001':
                    with patch.object(sys, 'stderr', FailedStderr()):
                        candidate.main(arguments)
                else:
                    with patch.object(candidate, 'pipe_json', side_effect=EOFError('injected pre-handshake EOF')):
                        candidate.main(arguments)
        finally:
            if output.is_dir():
                original_publish(output / 'injection-evidence.json', {'scenario': scenario, 'attempted': attempted})
        return
    parser = argparse.ArgumentParser()
    parser.add_argument('--role', choices=('builder', 'reviewer'), required=True)
    parser.add_argument('--inputs', required=True)
    parser.add_argument('--inputs-sha256', required=True)
    parser.add_argument('--r030-only', action='store_true')
    parser.add_argument('--shared-work-deadline-ns', type=int)
    parser.add_argument('--shared-cleanup-deadline-ns', type=int)
    args = parser.parse_args()
    global ROLE, CONTROL, TABLE, TABLE_SHA
    ROLE = args.role
    CONTROL = candidate.STAGE / ROLE / ('control/r030-supervisor-check-001' if args.r030_only else 'control/supervisor-check-001')
    started = DRIVER_STARTED
    deadline = started + (20 if args.r030_only else 29)
    shared_work = None
    if args.shared_work_deadline_ns is not None or args.shared_cleanup_deadline_ns is not None:
        if not args.r030_only or args.shared_work_deadline_ns is None or args.shared_cleanup_deadline_ns is None:
            raise ValueError('both shared deadlines require fixed three scenarios')
        shared_work = args.shared_work_deadline_ns / 1e9
        deadline = args.shared_cleanup_deadline_ns / 1e9
        if not time.monotonic() < shared_work < deadline or deadline - shared_work > 8:
            raise ValueError('invalid shared work/cleanup deadline')
    CONTROL.mkdir(exist_ok=False)
    environment = {'TMPDIR': str(candidate.STAGE / ROLE / 'tmp'), 'TMP': str(candidate.STAGE / ROLE / 'tmp'),
                   'TEMP': str(candidate.STAGE / ROLE / 'tmp'), 'XDG_CACHE_HOME': str(candidate.STAGE / ROLE / 'cache'),
                   'PYTHONDONTWRITEBYTECODE': '1', 'NO_PROXY': '127.0.0.1,localhost',
                   'LD_LIBRARY_PATH': str(candidate.REPOSITORY / '.cache/v0.5-s4/tools/root/usr/lib/x86_64-linux-gnu'), 'PATH': '/usr/bin:/bin'}
    TABLE = {'schema': 'r030-fixture-rows-v1' if args.r030_only else 'r029-fixture-rows-v1', 'rows': [{'role': ROLE, 'step': name, 'maximum_seconds': 12,
              'cwd': str(candidate.REPOSITORY), 'environment': environment,
              'argv': ['/usr/bin/python3', '-I', '-B', '-c', program], 'once': True, 'failure_stops_all': True}
              for name, program in candidate.TEST_WORK.items() if not args.r030_only or name in
              ('supervisor-normal-001', 'supervisor-publish-stderr-001', 'supervisor-pre-handshake-001')]}
    candidate.publish(CONTROL / 'fixture-rows.json', TABLE)
    TABLE_SHA = candidate.hash_file(CONTROL / 'fixture-rows.json')
    results = []
    failure = None
    previous = signal.getsignal(signal.SIGTERM)
    def term(signum, frame):
        raise TimeoutError('independent test driver TERM')
    signal.signal(signal.SIGTERM, term)
    try:
        candidate.verify_inputs(args.inputs, args.inputs_sha256)
        for path in (HERE / 'supervise.py', HERE / 'test_supervise.py'):
            compile(path.read_bytes(), str(path), 'exec')
        if not args.r030_only:
            unit = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(PureContracts))
            if not unit.wasSuccessful():
                raise ValueError('pure contracts failed')
        for row in TABLE['rows']:
            case_deadline = min(shared_work if shared_work is not None else deadline - 8, time.monotonic() + 4) if args.r030_only else deadline - 4
            if time.monotonic() >= case_deadline:
                raise TimeoutError('independent twelve-second work window exhausted')
            results.append(run_case(row, Path(args.inputs), args.inputs_sha256, case_deadline + 4 if args.r030_only else deadline, deadline if args.r030_only else None))
            if time.monotonic() >= deadline - 4:
                raise TimeoutError('driver deadline before next case')
        candidate.verify_inputs(args.inputs, args.inputs_sha256)
    except BaseException as error:
        failure = {'type': type(error).__name__, 'message': str(error)}
    finally:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        total_bytes = sum(path.stat().st_size for path in CONTROL.rglob('*') if path.is_file())
        if total_bytes > 4 * 1024 ** 2:
            failure = failure or {'message': 'supervisor check material exceeded4MiB'}
        candidate.publish(CONTROL / 'driver-exit.json', {'status': 'valid' if failure is None else 'invalid', 'first_error': failure,
             'pure_tests_planned': 0 if args.r030_only else 6, 'real_cli_cases_completed': len(results), 'results': results,
             'internal_elapsed_seconds': time.monotonic() - started, 'maximum_seconds': 20 if args.r030_only else 30, 'bytes_before_final_result': total_bytes,
             'shared_work_deadline_ns': args.shared_work_deadline_ns, 'shared_cleanup_deadline_ns': args.shared_cleanup_deadline_ns,
             'external_driver_wall_and_cleanup_required': True, 'dynamic_budget_not_authorized_by_file': True})
        signal.signal(signal.SIGTERM, previous)
    if failure is not None:
        raise RuntimeError('independent supervisor check failed; stop')


if __name__ == '__main__':
    main()
