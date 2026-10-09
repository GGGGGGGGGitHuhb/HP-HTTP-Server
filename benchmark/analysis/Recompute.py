#!/usr/bin/env python3
"""Independent S3 thread CPU and syscall raw consumer; no Run import."""
import argparse
import json
import hashlib
import math
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'benchmark/matrix'))
import Recompute as MatrixRecompute


def thread_values(before, after):
    if not before or len(before) != len(after):
        raise ValueError('missing thread raw')
    later = {r['tid']: r for r in after}
    if len(later) != len(after) or {r['tid'] for r in before} != set(later):
        raise ValueError('thread set changed')
    values = []
    for first in before:
        last = later[first['tid']]
        for key in ('pid', 'tid', 'starttime', 'clock_ticks_per_second'):
            if type(first[key]) is not int or first[key] <= 0 or first[key] != last[key]:
                raise ValueError('thread identity/frequency')
        for row in (first, last):
            if any(type(row[k]) is not int or row[k] < 0 for k in ('utime_ticks', 'stime_ticks')) or not math.isfinite(row['read_monotonic']):
                raise ValueError('thread raw values')
        wall = last['read_monotonic'] - first['read_monotonic']
        ticks = (last['utime_ticks'] + last['stime_ticks']) - (first['utime_ticks'] + first['stime_ticks'])
        if wall <= 0 or ticks < 0:
            raise ValueError('thread delta')
        seconds = ticks / first['clock_ticks_per_second']
        values.append({'tid': first['tid'], 'cpu_seconds': seconds, 'wall_s': wall, 'cpu_pct': seconds / wall * 100})
    return values


def syscall_values(text):
    rows, total = [], None
    for line in text.splitlines():
        fields = line.split()
        if len(fields) not in (5, 6) or not fields[0][:1].isdigit():
            continue
        percent, seconds, microseconds, calls = map(float, fields[:4])
        errors = float(fields[4]) if len(fields) == 6 else 0
        if not all(math.isfinite(n) and n >= 0 for n in (percent, seconds, microseconds, calls, errors)) or calls != int(calls) or errors != int(errors):
            raise ValueError('syscall scalar')
        row = {'syscall': fields[-1], 'system_seconds': seconds, 'calls': int(calls), 'errors': int(errors), 'percent_system_time': percent}
        if fields[-1] == 'total':
            if total is not None:
                raise ValueError('duplicate total')
            total = row
        else:
            rows.append(row)
    if not rows or total is None or total['calls'] <= 0 or sum(r['calls'] for r in rows) != total['calls'] or sum(r['errors'] for r in rows) != total['errors']:
        raise ValueError('missing/inconsistent syscall summary')
    if abs(sum(r['system_seconds'] for r in rows) - total['system_seconds']) > (len(rows) + 1) * 1e-6:
        raise ValueError('syscall time total')
    for row in rows:
        row['computed_percent_system_time'] = 100 * row['system_seconds'] / total['system_seconds'] if total['system_seconds'] else None
        tolerance = 0.02 + 100 * (len(rows) + 1) * 0.5e-6 / total['system_seconds'] if total['system_seconds'] else 0.02
        if row['computed_percent_system_time'] is not None and abs(row['computed_percent_system_time'] - row['percent_system_time']) > tolerance:
            raise ValueError('syscall rounded percent mismatch')
    return {'rows': sorted(rows, key=lambda r: r['system_seconds'], reverse=True), 'total': total}


def select_samples(suite, selection):
    if selection is None:
        if suite['counts'] != {'valid': 3, 'invalid': 0, 'not_run': 0} or len(suite['samples']) != 3:
            raise ValueError('incomplete S3 three-sample suite')
        return suite['samples']
    if selection != 'r001-valid-three':
        raise ValueError('unauthorized subset')
    if suite['counts'] != {'valid': 3, 'invalid': 1, 'not_run': 0} or len(suite['samples']) != 4:
        raise ValueError('source suite population')
    expected = [(1, 'M2', False, 'valid'), (2, 'M2', True, 'valid'), (3, 'M6', False, 'valid'), (4, 'M6', True, 'invalid')]
    if [(r['index'], r['id'], r['traced'], r['status']) for r in suite['samples']] != expected:
        raise ValueError('unauthorized selection identities')
    return suite['samples'][:3]


def file_sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for data in iter(lambda: stream.read(1048576), b''):
            digest.update(data)
    return digest.hexdigest()


def recompute(path, selection=None):
    path = pathlib.Path(path)
    suite = json.loads(path.read_text())
    if selection and path.resolve() != REPO / '.cache/v0.6-s3/builder/rework-001/suite/result.json':
        raise ValueError('selection source identity mismatch')
    selected = select_samples(suite, selection)
    expected_selected = [(1, 'M2', False), (2, 'M2', True), (3, 'M6', False)]
    if [(r['index'], r['id'], r['traced']) for r in selected] != expected_selected:
        raise ValueError('three-sample plan mismatch')
    output = {'status': 'valid_selection' if selection else 'passed', 'selection': selection,
              'source_suite_counts': suite['counts'], 'selected_counts': {'valid': 3},
              'omitted_invalid': [{'index': 4, 'id': 'M6', 'traced': True, 'missing': ['server after CPU', 'measurement cleanup/rusage', 'final metrics']} ] if selection else [],
              'product_identity': suite['product_identity'], 'source_hashes': {'result.json': file_sha(path)}, 'samples': []}
    for row in selected:
        directory = path.parent / f"{row['index']:02}-{row['id']}-{'traced' if row['traced'] else 'untraced'}"
        load = lambda name: json.loads((directory / name).read_text())
        dependencies = ['result.json', 'server.process.json', 'server.cleanup.json', 'measurement.process.json', 'measurement.cleanup.json', 'measurement.stdout', 'measurement.stderr']
        if row['traced']:
            dependencies += ['wrapper.process.json', 'wrapper.cleanup.json', 'wrapper.stdout', 'trace-summary.stderr']
        else:
            dependencies += ['server.stdout']
        for name in dependencies:
            output['source_hashes'][str((directory / name).relative_to(path.parent))] = file_sha(directory / name)
        measurement = row['measurement']
        lines = [line[len('MATRIX_SUMMARY '):] for line in (directory / 'measurement.stdout').read_text().splitlines() if line.startswith('MATRIX_SUMMARY ')]
        if len(lines) != 1:
            raise ValueError('missing raw scalar')
        scalar = json.loads(lines[0])
        if scalar['schema'] != 2 or scalar['population_status'] != 'not_collected' or scalar['corrected_population'] is not None or scalar['nonzero_bins'] is not None or any(scalar['errors'][k] != 0 for k in ('connect', 'read', 'write', 'status', 'timeout')):
            raise ValueError('invalid scalar')
        qps, mib = scalar['requests'] * 1e6 / scalar['duration_us'], scalar['bytes'] * 1e6 / scalar['duration_us'] / 1048576
        if not math.isclose(qps, measurement['summary']['qps'], rel_tol=1e-9) or not math.isclose(mib, measurement['summary']['received_mib_s'], rel_tol=1e-9):
            raise ValueError('derived throughput mismatch')
        if len(row['pre_audit']) != 3 or len(row['post_audit']) != 3 or row['metrics']['connections_active'] != 0:
            raise ValueError('HTTP/final accounting missing')
        server_identity = load('server.process.json')
        cpu = MatrixRecompute.cpu(measurement, server_identity, load('measurement.process.json'), load('measurement.cleanup.json'))
        if any(t['pid'] != server_identity['pid'] or not row['started_monotonic'] <= t['read_monotonic'] <= row['ended_monotonic'] for t in row['threads_before'] + row['threads_after']):
            raise ValueError('thread server identity/sample boundary')
        threads = thread_values(row['threads_before'], row['threads_after'])
        if len(row['thread_cpu']) != len(threads):
            raise ValueError('thread derived population')
        for original, expected in zip(row['thread_cpu'], threads):
            if original['tid'] != expected['tid'] or any(not math.isclose(original[k], expected[k], rel_tol=1e-9, abs_tol=1e-9) for k in ('cpu_seconds', 'wall_s', 'cpu_pct')):
                raise ValueError('thread derived mismatch')
        result = {'index': row['index'], 'id': row['id'], 'traced': row['traced'], 'cpu': cpu, 'thread_cpu': threads, 'qps': qps, 'received_mib_s': mib, 'latency_us': scalar['latency_us']}
        if row['traced']:
            trace = row['trace']
            if trace['trace_scope'] != 'server_lifecycle':
                raise ValueError('trace scope mismatch')
            keys = ('launch_requested_monotonic', 'tracee_confirmed_monotonic', 'ready_monotonic', 'warmup_completed_monotonic',
                    'measurement_started_monotonic', 'measurement_ended_monotonic', 'post_audit_completed_monotonic',
                    'server_term_requested_monotonic', 'wrapper_wait_completed_monotonic', 'tracee_disappearance_verified_monotonic')
            times = [trace[k] for k in keys]
            if not all(math.isfinite(t) for t in times) or times != sorted(times) or trace['measurement_started_monotonic'] != measurement['started_monotonic'] or trace['measurement_ended_monotonic'] != measurement['ended_monotonic']:
                raise ValueError('trace lifecycle ordering')
            wrapper = load('wrapper.process.json')
            if server_identity['pid'] == wrapper['pid'] or trace['relation']['ppid'] != wrapper['pid'] or trace['relation']['tracer_pid'] != wrapper['pid'] or server_identity['pid'] != trace['server_identity']['pid']:
                raise ValueError('trace wrapper/server identity')
            cleanup = load('server.cleanup.json')
            if cleanup['direct_child'] or not cleanup['tracee_disappeared'] or not cleanup['threads_disappeared'] or not cleanup['port_closed'] or not cleanup['wrapper_reaped'] or cleanup['forced']:
                raise ValueError('trace tree cleanup')
            result['syscalls'] = syscall_values((directory / 'trace-summary.stderr').read_text())
            if len(trace['summary']['rows']) != len(result['syscalls']['rows']):
                raise ValueError('syscall row population')
            for a, b in zip(trace['summary']['rows'], result['syscalls']['rows']):
                if any(a[key] != b[key] for key in b):
                    raise ValueError('syscall derived mismatch')
        output['samples'].append(result)
    output['pairs'] = {}
    for name in ('M2',):
        plain, traced = [r for r in suite['samples'] if r['id'] == name]
        output['pairs'][name] = {key: {'untraced': plain['measurement']['summary'][key], 'traced': traced['measurement']['summary'][key],
                                     'relative_change': traced['measurement']['summary'][key] / plain['measurement']['summary'][key] - 1}
                                 for key in ('qps', 'received_mib_s')}
    if any(file_sha(path.parent / name) != digest for name, digest in output['source_hashes'].items()):
        raise ValueError('source raw changed during selection')
    return output


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--suite', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--selection', choices=('r001-valid-three',))
    args = parser.parse_args()
    pathlib.Path(args.output).write_text(json.dumps(recompute(args.suite, args.selection), indent=2, allow_nan=False) + '\n')
    print('S3 independent raw CPU/syscall recompute passed: 3 samples')
