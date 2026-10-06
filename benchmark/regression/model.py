"""Frozen approved scenario schedule and acceptance arithmetic."""
import statistics
import math
import json
from identity import legacy

SCENARIOS = {
    'P1': {'size': 1024, 'workers': 0, 'threads': 1, 'connections': 1},
    'P2': {'size': 1024, 'workers': 2, 'threads': 2, 'connections': 32},
    'P3': {'size': 1024, 'workers': 4, 'threads': 2, 'connections': 128},
    'P4': {'size': 65536, 'workers': 2, 'threads': 2, 'connections': 32},
    'P5': {'size': 1048576, 'workers': 2, 'threads': 2, 'connections': 32},
}

def schedule(smoke=False):
    if smoke:
        return [{'round': 0, 'scenario': 'P2', 'label': 'D', **SCENARIOS['P2']}]
    result = []
    for number, order in enumerate((['P1','P2','P3','P4','P5'], ['P5','P4','P3','P2','P1'], ['P3','P4','P5','P1','P2']), 1):
        for scenario in order:
            for label in (('D','C') if number == 2 else ('C','D')):
                result.append({'round': number, 'scenario': scenario, 'label': label, **SCENARIOS[scenario]})
    return result

def server_args(item):
    args = list(legacy.SERVER_ARGS)
    args[1] = str(item['workers'])
    return args

def wrk_args(item):
    return [f"-t{item['threads']}", f"-c{item['connections']}"]

def aggregate(rows):
    expected = schedule()
    legacy.demand(len(rows) == 30, 'missing/extra sample')
    for row, item in zip(rows, expected):
        legacy.demand(row['schedule'] == item and row['status'] == 'valid' and row['label'] == item['label'], 'sample order/membership/status mismatch')
        payload = row['payload']
        legacy.demand(payload['size'] == item['size'], 'payload/scenario mismatch')
        for audit in ('pre_audit', 'post_audit'):
            legacy.demand(len(row[audit]) == 5 and all(a == {'status': 200, 'bytes': payload['size'], 'sha256': payload['sha256']} for a in row[audit]), 'audit evidence invalid')
        legacy.demand(row['cleanup']['reaped'] and not row['cleanup']['forced'] and row['cleanup']['returncode'] == 0, 'cleanup evidence invalid')
        legacy.demand(row['server_command'][1:1+len(server_args(item))] == server_args(item), 'actual server parameters mismatch')
        for phase in ('warmup', 'measurement'):
            metric = row[phase]
            legacy.demand(metric['command'][1:3] == wrk_args(item), 'actual wrk parameters mismatch')
            seconds = 5 if phase == 'warmup' else 20
            parsed = legacy.parse_summary(legacy.SUMMARY_PREFIX + json.dumps(metric), 0, seconds, item['size'])
            legacy.demand(math.isfinite(metric['qps']) and metric['qps'] > 0 and math.isclose(metric['qps'], parsed['qps'], rel_tol=1e-12), 'invalid derived QPS')
            legacy.demand(math.isfinite(metric['latency_ms']['p99']) and metric['latency_ms']['p99'] > 0 and math.isclose(metric['latency_ms']['p99'], parsed['latency_ms']['p99'], rel_tol=1e-12), 'invalid derived P99')
            for key in ('server_cpu_percent_one_core', 'wrk_cpu_percent_one_core', 'rss_sampled_max_kib'):
                legacy.demand(math.isfinite(metric['resources'][key]) and metric['resources'][key] >= 0, 'invalid resources')
    groups, comparison = {}, {}
    for scenario in SCENARIOS:
        for label in ('C', 'D'):
            selected = [r['measurement'] for r in rows if r['schedule']['scenario'] == scenario and r['label'] == label]
            qps = [r['qps'] for r in selected]
            median = statistics.median(qps)
            groups[scenario + label] = {'qps': median, 'p99_ms': statistics.median(r['latency_ms']['p99'] for r in selected), 'qps_span': (max(qps)-min(qps))/median,
                'server_cpu_percent_one_core': statistics.median(r['resources']['server_cpu_percent_one_core'] for r in selected),
                'wrk_cpu_percent_one_core': statistics.median(r['resources']['wrk_cpu_percent_one_core'] for r in selected),
                'rss_max_kib': max(r['resources']['rss_sampled_max_kib'] for r in selected)}
        c, d = groups[scenario+'C'], groups[scenario+'D']
        small = scenario in ('P1','P2','P3')
        comparison[scenario] = {'qps_ratio': d['qps']/c['qps'], 'p99_ratio': d['p99_ms']/c['p99_ms'], 'qps_min': 10 if small else .90, 'p99_max': .25 if small else 1.25}
    failed = [key + ': noisy' for key,g in groups.items() if g['qps_span'] > .20]
    failed += [key + ': numerical gate' for key,g in comparison.items() if g['qps_ratio'] < g['qps_min'] or g['p99_ratio'] > g['p99_max']]
    return {'groups': groups, 'comparison': comparison, 'failures': failed}
