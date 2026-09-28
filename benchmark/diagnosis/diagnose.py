#!/usr/bin/env python3
"""V0.5.1 isolated diagnostics; historical primitives stay unchanged."""
import argparse
import hashlib
import importlib.util
import json
import pathlib
import shutil
import statistics
import sys
import time

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'benchmark'))
import build as builds
import run as legacy

builds.COMMITS['C'] = '63c184297a01004c381d07fa5845cdd675a62739'
builds.TREES['C'] = 'cc5e9d443562aed00f7cd1f224f039fa0046ceff'


def role_root(role):
    return REPO / '.cache/v0.5.1-s1' / role


def checked_output(role, output):
    output = pathlib.Path(output).resolve()
    legacy.demand(output.is_relative_to(role_root(role)), 'output outside role root')
    legacy.demand(output.name.startswith('run-'), 'run output must use run- prefix')
    legacy.demand(not output.exists(), 'output must be new')
    output.mkdir(parents=True)
    return output


def identities():
    return {str(path.relative_to(REPO)): builds.sha(path) for path in
            [pathlib.Path(__file__), REPO/'benchmark/build.py', REPO/'benchmark/run.py', REPO/'benchmark/summary.lua']}


def summary(rows, labels):
    groups = {}
    legacy.demand(len(rows) == 6*len(labels), 'incomplete suite')
    for label in labels:
        for size in (1024, 1048576):
            group = [r for r in rows if r['label'] == label and r['payload']['size'] == size]
            legacy.demand(len(group) == 3 and all(r['status'] == 'valid' for r in group), 'invalid group')
            values = [r['measurement']['qps'] for r in group]
            median = statistics.median(values)
            span = (max(values)-min(values))/median
            groups[f'{label}-{size}'] = dict(qps_samples=values, qps_median=median,
                qps_relative_span=span, noisy=span > .2,
                median_of_run_p99_ms=statistics.median(r['measurement']['latency_ms']['p99'] for r in group))
    return groups


def stable_tool_identity(tool):
    return {key: tool[key] for key in ('sha256', 'libraries', 'version', 'version_exit')}


def accumulated_seconds(root):
    return sum(json.loads(p.read_text()).get('wall_seconds', 0) for p in root.glob('run-*/run.json'))


def run_suite(args):
    output = checked_output(args.role, args.output)
    root = role_root(args.role)
    result = dict(kind='formal', status='invalid', samples=[], started_utc=legacy.utc(),
                  tools=identities(), log_limit_bytes=legacy.LOG_LIMIT)
    start = time.monotonic()
    prior = accumulated_seconds(root)
    original_guard = legacy.log_guard
    def guard(_output):
        original_guard(root)
        legacy.demand(legacy.log_bytes(root)+sum(p.stat().st_size for p in root.rglob('*.trace')) <= legacy.LOG_LIMIT, 'trace/log cumulative budget')
        legacy.demand(prior + time.monotonic()-start <= 1800, 'role dynamic 30 minute budget')
    legacy.log_guard = guard
    try:
        result['environment'] = legacy.environment(output)
        guard(output)
        labels = ('A', 'B') if args.suite == 'AB' else ('C',)
        manifests = {label: legacy.validate_manifest(root/label/'manifest.json', label) for label in labels}
        result['builds'] = manifests
        tool = legacy.validate_tool(args.wrk)
        result['wrk'] = tool
        fixture_root = output/'root'
        fixture_root.mkdir()
        payloads = {size: legacy.fixture(fixture_root, size) for size in (1024, 1048576)}
        plan = legacy.schedule() if args.suite == 'AB' else [(r,s,'C') for r in (1,2,3) for s in ((1024,1048576) if r%2 else (1048576,1024))]
        result['order'] = plan
        legacy.save(output/'run.json', result)
        for index,(round_no,size,label) in enumerate(plan,1):
            row = legacy.run_sample(manifests[label], args.wrk, fixture_root, payloads[size],
                  output/f'sample-{index:02d}-{label}-{size}', output, min(start+600,start+1800-prior))
            row['round'] = round_no
            result['samples'].append(row)
            legacy.save(output/'run.json', result)
            print(f'{index}/{len(plan)} {label}/{size}: {row["measurement"]["qps"]:.2f}',flush=True)
        for label in labels:
            legacy.validate_manifest(root/label/'manifest.json',label)
        legacy.demand(identities() == result['tools'], 'diagnostic tool identity drift')
        legacy.demand(stable_tool_identity(legacy.validate_tool(args.wrk)) == stable_tool_identity(tool), 'wrk identity drift')
        guard(output)
        result['summary'] = summary(result['samples'], labels)
        result['status'] = 'valid'
        return 0
    except (Exception, KeyboardInterrupt) as error:
        result['error'] = repr(error)
        print(result['error'],file=sys.stderr)
        return 1
    finally:
        result['wall_seconds'] = time.monotonic()-start
        result['prior_dynamic_seconds'] = prior
        result['ended_utc'] = legacy.utc()
        result['observed_log_bytes_at_stop'] = legacy.log_bytes(root)
        legacy.save(output/'run.json', result)
        if (output/'root').exists():
            shutil.rmtree(output/'root')
        legacy.log_guard = original_guard


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--role',choices=('builder','reviewer'),required=True)
    sub = parser.add_subparsers(dest='action', required=True)
    sub.add_parser('build')
    run = sub.add_parser('run')
    run.add_argument('--suite',choices=('AB','C'),required=True)
    run.add_argument('--wrk',required=True)
    run.add_argument('--output',required=True)
    args = parser.parse_args()
    if args.action == 'build':
        for label in ('A','B','C'):
            builds.build(label,role_root(args.role)/label)
        return 0
    return run_suite(args)

if __name__ == '__main__':
    raise SystemExit(main())
