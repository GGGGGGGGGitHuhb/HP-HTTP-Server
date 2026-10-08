#!/usr/bin/env python3
"""Fast functional/negative tests; synthetic data is never performance evidence."""
import argparse
import copy
import hashlib
import json
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import tempfile
import time

REPO = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'benchmark/matrix'))
import Build
import Common
import Run
import Recompute


def rejects(function, message):
    try:
        function()
    except (ValueError, KeyError) as error:
        assert message in str(error), (message, repr(error))
        return str(error)
    raise AssertionError('negative case unexpectedly accepted')


def cpu_checks():
    server = {'pid': 100, 'starttime': 200}
    client = {'pid': 101, 'starttime': 201}
    before = {**server, 'utime_ticks': 100, 'stime_ticks': 50, 'clock_ticks_per_second': 100, 'cpu_seconds': 1.5, 'read_monotonic': 101}
    after = {**server, 'utime_ticks': 300, 'stime_ticks': 50, 'clock_ticks_per_second': 100, 'cpu_seconds': 3.5, 'read_monotonic': 103}
    usage = {**client, 'wait4_pid': 101, 'ru_utime_s': 0.2, 'ru_stime_s': 0.1, 'wait4_read_monotonic': 102}
    row = {'started_monotonic': 100, 'ended_monotonic': 104, 'wall_s': 4, 'server_cpu_pct': 50, 'client_cpu_pct': 7.5,
           'cpu_evidence': {'schema': 1, 'units': {'ticks': 'clock ticks', 'time': 'seconds', 'cpu_pct': 'one core = 100%'},
                            'server_before': before, 'server_after': after, 'client': usage}}
    cleanup = {**client, 'reaped': True, 'wait4': {'pid': 101, 'ru_utime_s': 0.2, 'ru_stime_s': 0.1, 'read_monotonic': 102}}
    assert Run.validate_cpu_evidence(row, server, client)['server_cpu_seconds_delta'] == 2
    assert Recompute.cpu(row, server, client, cleanup)['server_cpu_pct'] == 50
    negatives = []
    def rejected(mutator, label):
        bad = copy.deepcopy(row)
        mutator(bad)
        for consumer in (lambda: Run.validate_cpu_evidence(bad, server, client), lambda: Recompute.cpu(bad, server, client, cleanup)):
            try:
                consumer()
            except (ValueError, KeyError):
                pass
            else:
                raise AssertionError('CPU negative accepted: ' + label)
        negatives.append(label)
    for key in ('server_before', 'server_after', 'client'):
        rejected(lambda r, k=key: r['cpu_evidence'].pop(k), 'missing ' + key)
    for key in ('started_monotonic', 'ended_monotonic', 'wall_s'):
        rejected(lambda r, k=key: r.pop(k), 'missing ' + key)
    for target, key, value in (('server_before', 'pid', 999), ('server_after', 'starttime', 999),
                               ('client', 'pid', 999), ('client', 'starttime', 999), ('client', 'wait4_pid', 999),
                               ('server_after', 'clock_ticks_per_second', 0), ('server_before', 'utime_ticks', -1),
                               ('server_before', 'cpu_seconds', float('nan')), ('client', 'ru_utime_s', -1),
                               ('client', 'ru_stime_s', float('inf')), ('server_before', 'read_monotonic', 99),
                               ('server_after', 'read_monotonic', float('nan')), ('client', 'wait4_read_monotonic', 105)):
        rejected(lambda r, t=target, k=key, v=value: r['cpu_evidence'][t].__setitem__(k, v), target + '/' + key)
    rejected(lambda r: r['cpu_evidence']['server_after'].update(clock_ticks_per_second=200, cpu_seconds=1.75), 'inconsistent frequency')
    rejected(lambda r: r.__setitem__('wall_s', 3), 'wrong wall')
    rejected(lambda r: r.__setitem__('server_cpu_pct', 51), 'wrong server percentage')
    rejected(lambda r: r.__setitem__('client_cpu_pct', 8), 'wrong client percentage')
    rejected(lambda r: r['cpu_evidence']['client'].pop('ru_utime_s'), 'missing usage')
    return {'hand_calculation': {'server_ticks_delta': 200, 'frequency': 100, 'wall': 4, 'server_pct': 50, 'client_pct': 7.5}, 'negatives': negatives}


def synthetic_dispatch(root, real_budget):
    """Actual Run dispatch/sample/server/audit/finally; only load is synthetic."""
    output_root = Common.fresh_directory(root / 'synthetic-dispatch')
    class SyntheticBudget:
        def __init__(self, role, recovery=False):
            self.root = role
            self.output_root = output_root
            self.data = {'data_kind': 'synthetic functional failure; no formal slot', 'deadline_monotonic': real_budget.phase_deadline,
                         'phases': {'fastchecks': {'status': 'passed'}, 'smoke': {'status': 'passed'}}}
            self.phase_deadline = real_budget.phase_deadline
        def begin(self, phase):
            assert phase == 'formal'
            self.data['phases'][phase] = {'status': 'running'}
            Common.save(output_root / 'synthetic-budget.json', self.data)
        def check(self, deadline=None):
            real_budget.check(deadline)
        def finish(self, status, error=None):
            self.data['phases']['formal'] = {'status': status, 'error': error}
            Common.save(output_root / 'synthetic-budget.json', self.data)
    def mock_load(server, port, payload, config, seconds, prefix, budget):
        if prefix.parent.name.startswith('03-') and prefix.name == 'measurement':
            prefix.with_suffix('.stdout').write_text('SYNTHETIC injected third measurement failure; no wrk load\n')
            prefix.with_suffix('.stderr').write_text('')
            raise ValueError('synthetic third measurement failure')
        started = time.monotonic()
        before = Common.process_info(server.process.pid)
        child = Common.OwnedProcess([sys.executable, '-c', 'print("SYNTHETIC mock wrk; no load")'], prefix)
        try:
            while child.poll() is None:
                budget.check()
                time.sleep(0.01)
            after = Common.process_info(server.process.pid)
            cleanup = child.close(real_budget.data['deadline_monotonic'])
            ended = time.monotonic()
            wall = ended - started
            summary = {'schema': 2, 'duration_us': 1000000, 'requests': 100, 'bytes': (payload['size'] + 120) * 100,
                       'errors': dict.fromkeys(('connect', 'read', 'write', 'status', 'timeout'), 0),
                       'latency_us': {'mean': 10, 'p50': 8, 'p95': 12, 'p99': 20, 'max': 30},
                       'latency_distribution': 'wrk_corrected', 'population_status': 'not_collected', 'corrected_population': None,
                       'nonzero_bins': None, 'correction_interval_us': 10000 * config['connections']}
            parsed = Run.parse_summary('MATRIX_SUMMARY ' + json.dumps(summary), 0, payload['size'], config['connections'])
            result = {'summary': parsed, 'synthetic': True, 'started_monotonic': started, 'ended_monotonic': ended, 'wall_s': wall,
                      'server_cpu_pct': (after['cpu_seconds'] - before['cpu_seconds']) / wall * 100,
                      'client_cpu_pct': (child.usage.ru_utime + child.usage.ru_stime) / wall * 100,
                      'server_rss_sampled_max_kib': before['rss_kib'], 'client_rss_sampled_max_kib': child.identity['rss_kib'],
                      'rss_samples': [], 'cleanup': cleanup, 'cpu_evidence': Run.cpu_evidence(before, after, child)}
            Run.validate_cpu_evidence(result, server.identity, child.identity)
            return result
        finally:
            if child.process.returncode is None:
                child.close(real_budget.data['deadline_monotonic'])
    original_budget, original_load = Run.Budget, Run.wrk_phase
    Run.Budget, Run.wrk_phase = SyntheticBudget, mock_load
    try:
        rejects(lambda: Run.main(['--root', str(real_budget.root), '--mode', 'formal']), 'synthetic third measurement failure')
    finally:
        Run.Budget, Run.wrk_phase = original_budget, original_load
    suite = json.loads((output_root / 'suite-formal/result.json').read_text())
    assert suite['counts'] == {'valid': 2, 'invalid': 1, 'not_run': 15}
    assert [r['index'] for r in suite['samples']] == [1, 2, 3] and suite['not_run'][0]['index'] == 4
    for row in suite['samples']:
        directory = output_root / 'suite-formal' / f"{row['index']:02}-{row['id']}"
        assert row['server_cleanup']['reaped'] and row['metrics']['connections_active'] == 0
        assert len(row['pre_audit']) == 3
        assert json.loads((directory / 'result.json').read_text()) == row
        assert json.loads((directory / 'server.cleanup.json').read_text())['reaped']
        if row['status'] == 'valid':
            assert len(row['post_audit']) == 3
    assert suite['samples'][2]['status'] == 'invalid' and 'synthetic third' in suite['samples'][2]['error']
    assert len(list((output_root / 'suite-formal').glob('[0-9][0-9]-M*'))) == 3
    return {'data_kind': 'synthetic load, real Run dispatch/server/HTTP/finally', 'counts': suite['counts'], 'output': str(output_root), 'no_formal_slot_consumed': True}


def checks(root, real_budget=None):
    results = {'cpu_raw_input_negatives': cpu_checks()}
    fresh = root / 'driver-future'
    assert not fresh.exists()
    Common.fresh_directory(fresh)
    marker = fresh / 'owned.txt'
    marker.write_text('preserve')
    results['existing_output'] = rejects(lambda: Common.fresh_directory(fresh), 'already exists')
    assert marker.read_text() == 'preserve'
    command = [sys.executable, str(REPO / 'benchmark/matrix/Run.py'), '--root', str(root), '--mode', 'smoke']
    rejected = subprocess.run(command, capture_output=True, text=True)
    assert rejected.returncode != 0 and 'role root mismatch' in rejected.stderr
    (root / 'negative-entry.stderr').write_text(rejected.stderr)
    results['actual_cli_invalid_root'] = {'argv': command, 'returncode': rejected.returncode, 'no_future_suite': not (root / 'suite-smoke').exists()}
    synthetic = {'schema': 2, 'duration_us': 1000000, 'requests': 100, 'bytes': 110000,
                 'errors': dict.fromkeys(('connect', 'read', 'write', 'status', 'timeout'), 0),
                 'latency_us': {'mean': 10, 'p50': 8, 'p95': 12, 'p99': 20, 'max': 30},
                 'latency_distribution': 'wrk_corrected', 'population_status': 'not_collected', 'corrected_population': None, 'nonzero_bins': None, 'correction_interval_us': 320000}
    text = 'MATRIX_SUMMARY ' + json.dumps(synthetic)
    assert Run.parse_summary(text, 0, 1024, 32)['qps'] == 100
    rejects(lambda: Run.parse_summary('', 0, 1024, 32), 'missing')
    rejects(lambda: Run.parse_summary(text + '\n' + text, 0, 1024, 32), 'duplicate summary')
    bad = copy.deepcopy(synthetic)
    bad['latency_us']['mean'] = float('nan')
    rejects(lambda: Run.parse_summary('MATRIX_SUMMARY ' + json.dumps(bad), 0, 1024, 32), 'nonfinite')
    bad = copy.deepcopy(synthetic)
    bad['errors']['timeout'] = 1
    rejects(lambda: Run.parse_summary('MATRIX_SUMMARY ' + json.dumps(bad), 0, 1024, 32), 'measurement errors')
    rejects(lambda: Run.parse_summary(text.replace('"requests": 100', '"requests": 100, "requests": 100'), 0, 1024, 32), 'duplicate JSON field')
    results['summary_negatives'] = ['missing', 'duplicate', 'nonfinite', 'errors', 'duplicate JSON field']
    for key in ('corrected_population', 'nonzero_bins'):
        bad = copy.deepcopy(synthetic)
        bad[key] = 0
        rejects(lambda: Run.parse_summary('MATRIX_SUMMARY ' + json.dumps(bad), 0, 1024, 32), 'explicit null')
        del bad[key]
        rejects(lambda: Run.parse_summary('MATRIX_SUMMARY ' + json.dumps(bad), 0, 1024, 32), key)
    bad = copy.deepcopy(synthetic)
    bad['population_status'] = 'requests'
    rejects(lambda: Run.parse_summary('MATRIX_SUMMARY ' + json.dumps(bad), 0, 1024, 32), 'metadata')
    rows = [{'id': name, 'index': index + 1, 'repeat': 1, 'status': 'valid' if index < 2 else 'invalid'} for index, name in enumerate(Run.SEQUENCE[:3])]
    counts, tail = Run.sample_accounting(Run.SEQUENCE, rows)
    assert counts == {'valid': 2, 'invalid': 1, 'not_run': 15} and tail[0]['index'] == 4
    results['schema2_population_and_failed_third'] = counts
    fake_root = Common.fresh_directory(root / 'synthetic-budget')
    budget = Common.Budget(fake_root, create=True)
    budget.begin('fastchecks')
    rejects(lambda: budget.begin('fastchecks'), 'already consumed')
    original_deadline = budget.phase_deadline
    budget.phase_deadline = time.monotonic() - 1
    rejects(budget.check, 'deadline')
    budget.phase_deadline = original_deadline
    (fake_root / 'overflow.stderr').write_bytes(b'x' * 100)
    budget.data['limits']['raw_bytes'] = 10
    budget.last_scan = 0
    rejects(budget.check, 'raw output limit')
    budget.data['limits']['raw_bytes'] = 1000
    budget.data['limits']['task_bytes'] = 10
    budget.last_scan = 0
    rejects(budget.check, 'task space')
    results['synthetic_budget_negatives'] = ['single phase', 'expired deadline', 'raw overshoot90B', 'task overshoot']
    # Actual owned process boundaries, without a load generator.
    early = Common.OwnedProcess([sys.executable, '-c', 'raise SystemExit(7)'], root / 'early')
    while early.poll() is None:
        time.sleep(0.01)
    results['early_exit'] = early.close()
    assert results['early_exit']['returncode'] == 7 and results['early_exit']['reaped']
    child = Common.OwnedProcess([sys.executable, '-c', 'import time; time.sleep(30)'], root / 'identity')
    saved = dict(child.identity)
    child.identity['starttime'] += 1
    try:
        rejects(child.close, 'PID/starttime')
        assert child.poll() is None
    finally:
        child.identity = saved
        results['pid_restore_cleanup'] = child.close()
    forced = Common.OwnedProcess([sys.executable, '-c', 'import signal,time; signal.signal(signal.SIGTERM,signal.SIG_IGN); print("ready",flush=True); time.sleep(30)'], root / 'force')
    end = time.monotonic() + 2
    while 'ready' not in forced.stdout_path.read_text():
        assert time.monotonic() < end
        time.sleep(0.01)
    results['force_cleanup'] = forced.close(time.monotonic() + 0.5)
    assert results['force_cleanup']['forced'] and results['force_cleanup']['reaped']
    if real_budget:
        lua = (REPO / 'benchmark/matrix/Summary.lua').read_text()
        assert 'latency(i)' not in lua and '#latency' not in lua and 'for i' not in lua
        if real_budget.recovery:
            snapshot = copy.copy(real_budget)
            snapshot.data = copy.deepcopy(real_budget.data)
            snapshot.phase_deadline = time.monotonic() - 1
            rejects(lambda: snapshot.begin('formal'), 'deadline')
            snapshot.data['phases']['formal'] = {'status': 'invalid'}
            rejects(lambda: snapshot.begin('formal'), 'already consumed')
            assert Common.sha(real_budget.root / 'budget.json') == real_budget.data['recovery']['old_budget_sha256']
            assert Common.sha(real_budget.root / 'budget-settlement.json') == real_budget.data['recovery']['old_settlement_sha256']
            results['recovery_negatives'] = ['expired deadline', 'consumed formal', 'old ledgers unchanged', 'no Lua population scan']
        results['real_dispatch_third_failure'] = synthetic_dispatch(root, real_budget)
        manifest_path = real_budget.root / 'artifact/manifest.json'
        manifest = Build.validate_manifest(manifest_path)
        bad = copy.deepcopy(manifest)
        bad['commit'] = '0' * 40
        target = root / 'bad-manifest.json'
        Common.save(target, bad)
        rejects(lambda: Build.validate_manifest(target), 'fixed identity')
        binary = root / 'corrupt-elf'
        binary.write_bytes(pathlib.Path(manifest['binary']).read_bytes() + b'changed')
        bad = copy.deepcopy(manifest)
        bad['binary'] = str(binary)
        Common.save(target, bad)
        rejects(lambda: Build.validate_manifest(target), 'binary drift')
        source_copy = root / 'source-copy'
        shutil.copytree(manifest['source'], source_copy)
        (source_copy / 'CMakeLists.txt').write_text('drift')
        bad = copy.deepcopy(manifest)
        bad['source'] = str(source_copy)
        Common.save(target, bad)
        rejects(lambda: Build.validate_manifest(target), 'source drift')
        results['real_manifest_negatives'] = ['fixed commit', 'ELF drift', 'source drift']
        payload_dir = Common.fresh_directory(root / 'audit-payload')
        payload_file = payload_dir / 'payload.bin'
        payload_file.write_bytes(bytes(range(256)) * 4)
        payload = {'size': 1024, 'name': payload_file.name, 'sha256': Common.sha(payload_file)}
        server = Common.OwnedProcess([manifest['binary'], '--port', '0', '--root', str(payload_dir), '--threads', '0', '--metrics-on-exit'], root / 'audit-server')
        try:
            port = Run.ready(server, real_budget)
            results['keepalive_audit'] = Run.audit(port, payload, 'keepalive', real_budget)
            results['short_audit'] = Run.audit(port, payload, 'short', real_budget)
            original_phase = Run.wrk_phase
            def interrupt_phase(*args, **kwargs):
                raise KeyboardInterrupt('synthetic controller interruption')
            Run.wrk_phase = interrupt_phase
            case = Common.fresh_directory(root / 'interrupt-sample')
            try:
                try:
                    Run.sample(manifest, Run.MATRIX['M2'], case, payload_dir, payload, real_budget, 1, 1)
                except KeyboardInterrupt:
                    pass
                else:
                    raise AssertionError('interrupt disappeared')
            finally:
                Run.wrk_phase = original_phase
            interrupted = json.loads((case / 'result.json').read_text())
            assert interrupted['status'] == 'invalid' and interrupted['server_cleanup']['reaped']
            results['interrupt_sample_cleanup'] = interrupted['server_cleanup']
        finally:
            results['audit_server_cleanup'] = server.close(real_budget.data['deadline_monotonic'])
        metrics = Run.parse_metrics(server.stdout_path.read_text())
        assert metrics['requests_started_total'] == metrics['responses_completed_total'] == 6
        results['real_audit_metrics'] = metrics
        failed = Common.OwnedProcess([sys.executable, '-c', 'raise SystemExit(3)'], root / 'readiness-fail')
        try:
            rejects(lambda: Run.ready(failed, real_budget), 'before readiness')
        finally:
            results['readiness_fail_cleanup'] = failed.close(real_budget.data['deadline_monotonic'])
    Common.save(root / 'result.json', {'status': 'passed', 'data_kind': 'functional/synthetic negatives; not performance samples', 'cases': results})
    return results


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--role-root')
    parser.add_argument('--recovery', action='store_true')
    parser.add_argument('--recovery-name', choices=('rework-001', 'rework-002'))
    args = parser.parse_args()
    recovery = args.recovery_name or args.recovery
    budget = Common.Budget(Common.role_root(args.role_root), create=bool(recovery), recovery=recovery) if args.role_root else None
    if budget:
        budget.begin('fastchecks')
        root = Common.fresh_directory(budget.output_root / 'fastchecks')
    else:
        temporary = pathlib.Path(os.environ.get('HP_MATRIX_TEST_TMP_ROOT', REPO / '.cache/v0.6-s2/tests'))
        temporary.mkdir(parents=True, exist_ok=True)
        root = pathlib.Path(tempfile.mkdtemp(prefix='matrix-functional-', dir=temporary))
    try:
        cases = checks(root, budget)
        if budget:
            budget.check()
            budget.finish('passed')
        print('matrix fastchecks passed; cases=', len(cases), '; raw=', root)
    except BaseException as error:
        Common.save(root / 'failure.json', {'status': 'invalid', 'error': repr(error)})
        if budget:
            budget.finish('invalid', repr(error))
        raise


if __name__ == '__main__':
    main()
