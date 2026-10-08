#!/usr/bin/env python3
"""Independent raw-input consumer; does not import Run/Common CPU formulas."""
import argparse
import json
import math
import pathlib
import statistics


def require(condition, message):
    if not condition:
        raise ValueError(message)


def cpu(measurement, server, client, cleanup):
    raw = measurement['cpu_evidence']
    require(raw['schema'] == 1 and raw['units'] == {'ticks': 'clock ticks', 'time': 'seconds', 'cpu_pct': 'one core = 100%'}, 'CPU schema/units')
    start, end, wall = (measurement[key] for key in ('started_monotonic', 'ended_monotonic', 'wall_s'))
    require(all(type(v) in (int, float) and math.isfinite(v) and v >= 0 for v in (start, end, wall)) and wall > 0, 'CPU finite clock')
    require(end > start and math.isclose(end - start, wall, rel_tol=1e-9, abs_tol=1e-9), 'CPU wall mismatch')
    before, after = raw['server_before'], raw['server_after']
    for snapshot in (before, after):
        require((snapshot['pid'], snapshot['starttime']) == (server['pid'], server['starttime']), 'CPU server identity')
        for name in ('pid', 'starttime', 'utime_ticks', 'stime_ticks', 'clock_ticks_per_second'):
            require(type(snapshot[name]) is int and snapshot[name] >= (0 if name.endswith('_ticks') else 1), 'CPU tick input')
        require(type(snapshot['read_monotonic']) in (int, float) and math.isfinite(snapshot['read_monotonic']), 'CPU snapshot clock')
        seconds = (snapshot['utime_ticks'] + snapshot['stime_ticks']) / snapshot['clock_ticks_per_second']
        require(type(snapshot['cpu_seconds']) in (int, float) and math.isfinite(snapshot['cpu_seconds']) and math.isclose(snapshot['cpu_seconds'], seconds, rel_tol=1e-9, abs_tol=1e-9), 'CPU seconds conversion')
    require(before['clock_ticks_per_second'] == after['clock_ticks_per_second'], 'CPU frequency mismatch')
    require(start <= before['read_monotonic'] <= after['read_monotonic'] <= end, 'CPU snapshot ordering')
    delta_ticks = after['utime_ticks'] + after['stime_ticks'] - before['utime_ticks'] - before['stime_ticks']
    require(delta_ticks >= 0, 'CPU negative delta')
    usage = raw['client']
    require((usage['pid'], usage['starttime']) == (client['pid'], client['starttime']) and usage['wait4_pid'] == client['pid'], 'CPU client identity')
    require(all(type(usage[k]) is int and usage[k] > 0 for k in ('pid', 'starttime', 'wait4_pid')), 'CPU client identity type')
    require(all(type(usage[k]) in (int, float) and math.isfinite(usage[k]) and usage[k] >= 0 for k in ('ru_utime_s', 'ru_stime_s', 'wait4_read_monotonic')), 'CPU invalid usage')
    require(start <= usage['wait4_read_monotonic'] <= end, 'CPU wait4 ordering')
    require(cleanup['pid'] == client['pid'] and cleanup['starttime'] == client['starttime'] and cleanup['reaped'], 'CPU cleanup identity')
    require(cleanup['wait4'] == {'pid': usage['wait4_pid'], 'ru_utime_s': usage['ru_utime_s'], 'ru_stime_s': usage['ru_stime_s'], 'read_monotonic': usage['wait4_read_monotonic']}, 'CPU cleanup usage')
    derived = {'server_cpu_pct': delta_ticks / before['clock_ticks_per_second'] / wall * 100,
               'client_cpu_pct': (usage['ru_utime_s'] + usage['ru_stime_s']) / wall * 100}
    for key, value in derived.items():
        require(type(measurement[key]) in (int, float) and math.isfinite(measurement[key]) and math.isclose(value, measurement[key], rel_tol=1e-9, abs_tol=1e-9), 'CPU percentage mismatch')
    return derived


def recompute(path):
    path = pathlib.Path(path)
    suite = json.loads(path.read_text())
    require(suite['counts'] == {'valid': 18, 'invalid': 0, 'not_run': 0} and len(suite['samples']) == 18, 'incomplete suite')
    rows, groups = [], {}
    for sample in suite['samples']:
        directory = path.parent / f"{sample['index']:02}-{sample['id']}"
        measurement = sample['measurement']
        load = lambda name: json.loads((directory / name).read_text())
        result = cpu(measurement, load('server.process.json'), load('measurement.process.json'), load('measurement.cleanup.json'))
        lines = [line[len('MATRIX_SUMMARY '):] for line in (directory / 'measurement.stdout').read_text().splitlines() if line.startswith('MATRIX_SUMMARY ')]
        require(len(lines) == 1, 'summary count')
        summary = json.loads(lines[0])
        require(summary['schema'] == 2 and summary['latency_distribution'] == 'wrk_corrected' and summary['population_status'] == 'not_collected' and summary['corrected_population'] is None and summary['nonzero_bins'] is None, 'summary metadata')
        require(all(summary['errors'][key] == 0 for key in ('connect', 'read', 'write', 'status', 'timeout')), 'summary errors')
        result.update(qps=summary['requests'] * 1e6 / summary['duration_us'], received_mib_s=summary['bytes'] * 1e6 / summary['duration_us'] / 1048576,
                      server_rss_sampled_max_kib=max([measurement['cpu_evidence']['server_before']['rss_kib']] + [r['server_rss_kib'] for r in measurement['rss_samples']]))
        for name in ('mean', 'p50', 'p95', 'p99', 'max'):
            result['latency_' + name + '_us'] = summary['latency_us'][name]
        rows.append({'index': sample['index'], 'id': sample['id'], **result})
    for identifier, actual in suite['groups'].items():
        selected = [row for row in rows if row['id'] == identifier]
        require(len(selected) == 3, 'repeat population')
        groups[identifier] = {}
        for name, saved in actual.items():
            values = [row[name] for row in selected]
            median = statistics.median(values)
            expected = {'values': values, 'median': median, 'min': min(values), 'max': max(values), 'span_over_median': (max(values) - min(values)) / median if median else None}
            for key in ('median', 'min', 'max', 'span_over_median'):
                require(saved[key] == expected[key] if expected[key] is None else math.isclose(saved[key], expected[key], rel_tol=1e-9, abs_tol=1e-9), 'group statistic ' + name + '/' + key)
            require(all(math.isclose(a, b, rel_tol=1e-9, abs_tol=1e-9) for a, b in zip(saved['values'], values)), 'group values')
            groups[identifier][name] = expected
    return {'status': 'passed', 'independent_raw_input_cpu_samples': 18, 'samples': rows, 'groups': groups}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--suite', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    result = recompute(args.suite)
    pathlib.Path(args.output).write_text(json.dumps(result, indent=2, allow_nan=False) + '\n')
    print('independent recompute passed: 18 CPU inputs and 60 group statistics')
