#!/usr/bin/env python3
"""Functional S3 negatives; synthetic load is not a performance sample."""
import argparse
import copy
import importlib.util
import json
import os
import pathlib
import sys
import tempfile
import time

REPO = pathlib.Path(__file__).resolve().parents[1]

def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result

Run = module('S3Run', REPO / 'benchmark/analysis/Run.py')
Independent = module('S3Independent', REPO / 'benchmark/analysis/Recompute.py')
M = Run.M


def rejected(function, fragment):
    try:
        function()
    except (ValueError, KeyError, FileNotFoundError) as error:
        assert fragment in str(error), (fragment, repr(error))
        return
    raise AssertionError('unexpected acceptance: ' + fragment)


def synthetic_dispatch(root, role, outer, plan):
    isolated = M.fresh_directory(root / ('synthetic-third-' + str(len(plan))))
    original_identity = Run.identity
    manifest, tool = original_identity(role)
    class SyntheticBudget:
        def __init__(self, path):
            self.data = {'deadline_monotonic': outer.phase_deadline, 'phases': {'fastchecks': {'status': 'passed'}, 'probe': {'status': 'passed'}}}
            self.phase_deadline = outer.phase_deadline
        def begin(self, phase):
            self.data['phases'][phase] = {'status': 'running'}
        def check(self, deadline=None):
            outer.check(deadline)
        def finish(self, status, error=None):
            self.data['phases']['formal'] = {'status': status, 'error': error}
            M.save(isolated / 'synthetic-budget.json', {'data_kind': 'functional only; no formal slot', **self.data})
    def load(server, port, payload, config, seconds, prefix, budget):
        if prefix.parent.name.startswith('03-') and prefix.name == 'measurement':
            prefix.with_suffix('.stderr').write_text('synthetic third failure, no wrk or strace\n')
            raise ValueError('synthetic third failure')
        started = time.monotonic()
        before = M.process_info(server.identity['pid'])
        child = M.OwnedProcess([sys.executable, '-c', 'print("synthetic S3 load; no wrk/trace")'], prefix)
        try:
            while child.poll() is None:
                budget.check()
                time.sleep(0.01)
            after = M.process_info(server.identity['pid'])
            cleanup = child.close(outer.data['deadline_monotonic'])
            ended = time.monotonic()
            wall = ended - started
            summary = {'qps': 100, 'received_mib_s': 1, 'latency_us': {'mean': 1, 'p50': 1, 'p95': 1, 'p99': 1, 'max': 1}}
            measurement = {'synthetic': True, 'summary': summary, 'started_monotonic': started, 'ended_monotonic': ended, 'wall_s': wall,
                           'server_cpu_pct': (after['cpu_seconds'] - before['cpu_seconds']) / wall * 100,
                           'client_cpu_pct': (child.usage.ru_utime + child.usage.ru_stime) / wall * 100,
                           'cpu_evidence': Run.MatrixRun.cpu_evidence(before, after, child), 'cleanup': cleanup}
            Run.MatrixRun.validate_cpu_evidence(measurement, server.identity, child.identity)
            return measurement
        finally:
            if child.poll() is None:
                child.close(outer.data['deadline_monotonic'])
    def sample(manifest, config, traced, directory, payload_root, payload, budget):
        # Trace capability is only tested by the one actual probe, never here.
        return Run.sample(manifest, config, False, directory, payload_root, payload, budget, synthetic_load=load)
    original_budget, original_formal = Run.Budget, Run.formal
    Run.Budget = SyntheticBudget
    Run.identity = lambda path: (manifest, tool)
    Run.formal = lambda role, budget: original_formal(isolated, budget, sample_function=sample, plan=plan)
    try:
        rejected(lambda: Run.formal(role, SyntheticBudget(role)), 'synthetic third failure')
    finally:
        Run.Budget, Run.formal, Run.identity = original_budget, original_formal, original_identity
    suite = json.loads((isolated / 'suite/result.json').read_text())
    assert suite['counts'] == {'valid': 2, 'invalid': 1, 'not_run': len(plan) - 3}
    assert not suite['not_run'] if len(plan) == 3 else suite['not_run'][0]['index'] == 4
    for row in suite['samples']:
        assert row['server_cleanup']['reaped'] and row['metrics']['connections_active'] == 0 and len(row['pre_audit']) == 3
    assert len(list((isolated / 'suite').glob('[0-9][0-9]-M*'))) == 3
    return {'counts': suite['counts'], 'kind': 'actual dispatch/server/HTTP/finally; synthetic load, trace not exercised', 'isolated': str(isolated)}


def after_failure_checks(root, budget):
    """Real cached-wait4 child, then missing server after; no wrk/strace."""
    outcomes = {}
    class Server:
        process = type('Process', (), {'pid': os.getpid()})()
        identity = M.process_info(os.getpid())
        def poll(self): return None
        def matches(self): return True
    original_wait4 = M.os.wait4
    reaped_pids = []
    def counted_wait4(pid, options):
        result = original_wait4(pid, options)
        if result[0]:
            reaped_pids.append(result[0])
        return result
    M.os.wait4 = counted_wait4
    try:
        for state in ('already_polled_exit', 'live_child'):
            directory = M.fresh_directory(root / state)
            reads = [0]
            def missing_after(pid):
                reads[0] += 1
                if reads[0] > 1:
                    raise FileNotFoundError('synthetic server after read missing')
                return M.process_info(pid)
            def child_factory(argv, prefix, environment):
                child = M.OwnedProcess(argv, prefix, environment)
                if state == 'already_polled_exit':
                    while child.poll() is None:
                        budget.check()
                        time.sleep(0.01)
                return child
            command = [sys.executable, '-c', 'raise SystemExit(0)' if state == 'already_polled_exit' else 'import time;time.sleep(30)']
            rejected(lambda: Run.load_phase(Server(), 0, {'name': 'synthetic', 'size': 1024}, Run.MatrixRun.MATRIX['M2'], 1, directory / 'measurement', budget,
                                           command=command, server_reader=missing_after, child_factory=child_factory), 'synthetic server after')
            row = json.loads((directory / 'measurement.result.json').read_text())
            cleanup = json.loads((directory / 'measurement.cleanup.json').read_text())
            assert row['status'] == 'invalid' and row['server_after'] is None and 'server_cpu_pct' not in row
            assert cleanup['reaped'] and not cleanup['forced'] and cleanup['wait4']['pid'] == cleanup['pid']
            assert reaped_pids.count(cleanup['pid']) == 1
            assert cleanup['returncode'] == (0 if state == 'already_polled_exit' else -15)
            assert cleanup['wait4']['ru_utime_s'] >= 0 and cleanup['wait4']['ru_stime_s'] >= 0
            assert cleanup['wait4']['read_monotonic'] <= row['ended_monotonic']
            outcomes[state] = {'pid': cleanup['pid'], 'returncode': cleanup['returncode'], 'one_actual_wait4_reap': True, 'after': None, 'reaped': True}
    finally:
        M.os.wait4 = original_wait4
    return outcomes


def checks(root, budget=None, role=None):
    results = {}
    a = {'pid': 10, 'tid': 10, 'starttime': 20, 'utime_ticks': 100, 'stime_ticks': 50, 'clock_ticks_per_second': 100,
         'read_monotonic': 1, 'comm': 'example', 'role': 'main reactor'}
    b = {**a, 'utime_ticks': 200, 'read_monotonic': 3}
    assert Run.thread_cpu([a], [b])[0]['cpu_pct'] == 50
    assert Independent.thread_values([a], [b])[0]['cpu_pct'] == 50
    for consumer in (Run.thread_cpu, Independent.thread_values):
        rejected(lambda: consumer([a], []), 'missing thread')
        rejected(lambda: consumer([a], [{**b, 'starttime': 99}]), 'identity')
        rejected(lambda: consumer([a], [{**b, 'utime_ticks': 0}]), 'delta')
        rejected(lambda: consumer([a], [{**b, 'read_monotonic': float('nan')}]), 'raw' if consumer == Independent.thread_values else 'clock')
        rejected(lambda: consumer([{k:v for k,v in a.items() if k != 'starttime'}], [b]), 'starttime')
    results['thread_hand_and_negatives'] = ['50%', 'missing thread', 'missing identity', 'reuse identity', 'negative delta', 'nonfinite clock']
    text = '50.00 0.000010 1 10 2 read\n50.00 0.000010 1 10 write\n100.00 0.000020 1 20 2 total\n'
    assert Run.trace_summary(text)['total']['calls'] == Independent.syscall_values(text)['total']['calls'] == 20
    for parser in (Run.trace_summary, Independent.syscall_values):
        rejected(lambda: parser(''), 'missing')
        rejected(lambda: parser(text.replace('0.000010', '-0.000010', 1)), 'scalar')
    results['summary_negatives'] = ['empty', 'negative scalar', 'totals/errors20/2']
    source = {'counts': {'valid': 3, 'invalid': 1, 'not_run': 0}, 'samples': [{'index': i + 1, 'id': name, 'traced': traced, 'status': 'valid' if i < 3 else 'invalid'} for i, (name, traced) in enumerate(Run.LEGACY_SEQUENCE)]}
    assert len(Independent.select_samples(source, 'r001-valid-three')) == 3
    rejected(lambda: Independent.select_samples(source, 'first-two'), 'unauthorized subset')
    rejected(lambda: Independent.select_samples({**source, 'counts': {'valid': 4, 'invalid': 0, 'not_run': 0}}, 'r001-valid-three'), 'population')
    rejected(lambda: Independent.recompute(root / 'missing-result.json', 'r001-valid-three'), 'missing-result')
    results['explicit_selection'] = ['old source3/1/0 -> selected3valid', 'unapproved subset/population', 'missing source raw']
    fresh = M.fresh_directory(root / 'fresh')
    (fresh / 'marker').write_text('preserved')
    rejected(lambda: M.fresh_directory(fresh), 'already exists')
    rejected(lambda: Run.main(['--root', str(root), '--mode', 'formal']), 'role root mismatch')
    results['fresh_actual_entry'] = 'future driver creates; occupied output and wrong root rejected'
    server_identity, wrapper_identity = {'pid': 100, 'starttime': 200}, {'pid': 99, 'starttime': 199}
    expected = ['/verified/server', '--port', '0']
    Run.validate_relation(server_identity, wrapper_identity, {'ppid': 99, 'tracer_pid': 99}, '/verified/server', expected, expected)
    rejected(lambda: Run.validate_relation(wrapper_identity, wrapper_identity, {'ppid': 99, 'tracer_pid': 99}, '/verified/server', expected, expected), 'wrapper mistaken')
    rejected(lambda: Run.validate_relation(server_identity, wrapper_identity, {'ppid': 98, 'tracer_pid': 99}, '/verified/server', expected, expected), 'relation')
    rejected(lambda: Run.validate_relation(server_identity, wrapper_identity, {'ppid': 99, 'tracer_pid': 99}, '/wrong/server', expected, expected), 'exe/argv')
    scope = {'trace_scope': 'server_lifecycle', **dict(zip(('launch_requested_monotonic', 'tracee_confirmed_monotonic', 'ready_monotonic', 'post_audit_completed_monotonic', 'server_term_requested_monotonic', 'wrapper_wait_completed_monotonic', 'tracee_disappearance_verified_monotonic'), range(7)))}
    Run.validate_scope(scope)
    rejected(lambda: Run.validate_scope({**scope, 'trace_scope': 'measurement'}), 'scope')
    rejected(lambda: Run.validate_scope({k:v for k,v in scope.items() if k != 'ready_monotonic'}), 'ready_monotonic')
    results['wrapper_identity_scope'] = ['wrapper is not server', 'PPid/TracerPid mismatch', 'exe drift', 'missing scope/boundary']
    # Synthetic early-wrapper cleanup branch: no actual signal or orphan process.
    events, state = [], {'alive': True}
    class EarlyWrapper:
        identity = {'pid': 91827364, 'starttime': 100}
        def poll(self): return 1
        def close(self, deadline): return {'pid': 91827364, 'starttime': 100, 'returncode': 1, 'reaped': True, 'forced': False}
    early = object.__new__(Run.TracedServer)
    early.wrapper, early.identity = EarlyWrapper(), {'pid': 91827365, 'starttime': 101}
    early.directory, early.trace, early.port = M.fresh_directory(root / 'synthetic-early-wrapper'), {}, None
    early.budget = type('FakeBudget', (), {'data': {'deadline_monotonic': time.monotonic() + 3}})()
    early.matches = lambda: state['alive']
    saved_kill, saved_threads = Run.os.kill, Run.threads
    def recorded_signal(pid, sig):
        events.append([pid, sig])
        state['alive'] = False
    Run.os.kill = recorded_signal
    Run.threads = lambda pid: [{'tid': pid}]
    try:
        rejected(early.close, 'cleanup invalid')
    finally:
        Run.os.kill, Run.threads = saved_kill, saved_threads
    assert events == [[91827365, int(Run.signal.SIGTERM)]]
    result = json.loads((early.directory / 'server.cleanup.json').read_text())
    assert result['tracee_disappeared'] and result['threads_disappeared'] and not result['natural_wrapper_exit']
    results['early_wrapper_cleanup'] = {'kind': 'synthetic identity/signal branch; no orphan process', 'events': events, 'invalid_not_hidden': True}
    if budget:
        snapshot = copy.copy(budget)
        snapshot.data = copy.deepcopy(budget.data)
        snapshot.phase_deadline = time.monotonic() - 1
        rejected(snapshot.check, 'deadline')
        snapshot.data['phases']['formal'] = {'status': 'invalid'}
        rejected(lambda: snapshot.begin('formal'), 'consumed')
        no_probe = copy.copy(budget)
        no_probe.data = {'phases': {'probe': {'status': 'invalid'}}}
        rejected(lambda: Run.formal(role, no_probe), 'probe not passed')
        assert not (role / 'suite').exists()
        results['budget_probe_stop'] = ['absolute deadline', 'phase consumed', 'probe invalid prevents future suite']
        results['third_dispatch_new3'] = synthetic_dispatch(root, role, budget, Run.SEQUENCE)
        results['third_dispatch_legacy4'] = synthetic_dispatch(root, role, budget, Run.LEGACY_SEQUENCE)
        results['after_failure_cleanup'] = after_failure_checks(root, budget)
    child = M.OwnedProcess([sys.executable, '-c', 'import time;time.sleep(30)'], root / 'interrupt-child')
    try:
        raise KeyboardInterrupt('synthetic interruption')
    except KeyboardInterrupt:
        pass
    finally:
        results['interrupt_cleanup'] = child.close(budget.data['deadline_monotonic'] if budget else None)
    assert results['interrupt_cleanup']['reaped']
    M.save(root / 'result.json', {'status': 'passed', 'kind': 'functional/synthetic; no performance sample', 'cases': results})
    return results


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root')
    args = parser.parse_args()
    if args.root:
        role = Run.role_root(args.root)
        budget = Run.Budget(role, create=True)
        budget.begin('fastchecks')
        root = M.fresh_directory(role / 'fastchecks')
    else:
        budget = role = None
        directory = pathlib.Path(os.environ.get('HP_ANALYSIS_TEST_TMP_ROOT', REPO / '.cache/v0.6-s3/tests'))
        directory.mkdir(parents=True, exist_ok=True)
        root = pathlib.Path(tempfile.mkdtemp(prefix='analysis-functional-', dir=directory))
    try:
        cases = checks(root, budget, role)
        if budget:
            budget.finish('passed')
        print('S3 fastchecks passed; cases=', len(cases), '; raw=', root)
    except BaseException as error:
        M.save(root / 'failure.json', {'status': 'invalid', 'error': repr(error)})
        if budget:
            budget.finish('invalid', repr(error))
        raise


if __name__ == '__main__':
    main()
