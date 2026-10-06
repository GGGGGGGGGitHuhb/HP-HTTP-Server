#!/usr/bin/env python3
"""Fixed S3 regression CLI. No automatic retry, download or system tuning."""
import argparse
import json
import pathlib
import resource
import shutil
import subprocess
import sys
import time
import budget
import executor
import identity
import model
import supervision
from identity import legacy


def run_suite(args):
    root = identity.role_root(args.role)
    kind = 'smoke' if args.command == 'smoke' else 'formal'
    reservation = budget.Reservation(root, args.output, kind, 60 if kind == 'smoke' else 900)
    output = reservation.output
    record = {'schema': 1, 'run_id': reservation.id, 'kind': kind, 'status': 'invalid', 'started_utc': legacy.utc(), 'samples': [], 'schedule': model.schedule(kind == 'smoke'), 'prior_dynamic_seconds': reservation.previous}
    fixture_root = output / 'root'
    try:
        spec = json.loads((root / 'refs.json').read_text())
        legacy.demand(spec['commits']['C'] == identity.COMMITS['C'], 'baseline C must remain fixed')
        legacy.demand(spec['commits']['D'] == identity.COMMITS['D'] or bool(spec.get('requested_ref')), 'ad-hoc candidate requires explicit ref')
        identity.legacy_build.COMMITS.update(spec['commits'])
        identity.legacy_build.TREES.update(spec['trees'])
        formal = spec['commits'] == identity.COMMITS
        if not formal:
            record['kind'] = 'ad-hoc-smoke' if kind == 'smoke' else 'ad-hoc'
        record['refs'] = spec
        legacy.save(output / 'schedule.json', record['schedule'])
        reservation.guard()
        nofile = resource.getrlimit(resource.RLIMIT_NOFILE)[0]
        legacy.demand(nofile == resource.RLIM_INFINITY or nofile >= 1024, 'nofile below 1024')
        record['environment'] = legacy.environment(root)
        before = identity.snapshot(root, args.wrk)
        record['identity_before'] = before
        legacy.save(output / 'run.json', record)
        fixture_root.mkdir()
        fixtures = {size: legacy.fixture(fixture_root, size) for size in {item['size'] for item in record['schedule']}}
        deadline = reservation.began + reservation.allowance
        for index, item in enumerate(record['schedule'], 1):
            reservation.guard()
            row = executor.run_sample(before['manifests'][item['label']], args.wrk, fixture_root, fixtures[item['size']], output / f"{index:02d}-{item['scenario']}-{item['label']}", root, deadline, item, 1 if kind == 'smoke' else 5, 1 if kind == 'smoke' else 20)
            record['samples'].append(row)
            legacy.save(output / 'run.json', record)
        record['identity_after'] = identity.snapshot(root, args.wrk)
        legacy.demand(before == record['identity_after'], 'source/tool/compiler/library identity drift')
        reservation.guard()
        if kind != 'smoke':
            observed = model.aggregate(record['samples'])
            record['observed_comparison'] = observed
            if formal:
                legacy.demand(not observed['failures'], '; '.join(observed['failures']))
                record['summary'] = observed
                record['performance_acceptance'] = 'PASS'
            else:
                record['performance_acceptance'] = 'NOT_APPLICABLE_AD_HOC'
        else:
            record['performance_acceptance'] = 'NOT_APPLICABLE_SMOKE'
        record['status'] = 'valid'
    except BaseException as error:
        record['error'] = type(error).__name__ + ': ' + str(error)
        record.pop('summary', None)
        record.pop('performance_acceptance', None)
    finally:
        record['ended_utc'] = legacy.utc()
        record['wall_seconds'] = time.monotonic() - reservation.began
        record['role_log_bytes'] = legacy.log_bytes(root)
        legacy.save(output / 'run.json', record)
        reservation.finish()
        if fixture_root.exists():
            shutil.rmtree(fixture_root)
    print(json.dumps({'output': str(output), 'status': record['status'], 'samples': len(record['samples']), 'error': record.get('error')}), flush=True)
    return 0 if record['status'] == 'valid' else 1


def check(args):
    """Charge the independent eight CTest execution to the same dynamic budget."""
    root = identity.role_root(args.role)
    reservation = budget.Reservation(root, args.output, 'unit' if args.command == 'unit' else 'correctness', 180)
    record = {'run_id': reservation.id, 'kind': 'unit' if args.command == 'unit' else 'correctness', 'status': 'invalid', 'started_utc': legacy.utc()}
    try:
        command = ['ctest', '--test-dir', str(root / 'D/source/build-debug'), '--output-on-failure', '--timeout', '60']
        if args.command == 'unit':
            command = [sys.executable, str(identity.HERE / 'test_regression.py')]
        record['command'] = command
        reservation.guard()
        record['preflight'] = supervision.resources(root)
        record['cleanup'] = supervision.run(command, root, reservation.output / 'checks')
        reservation.guard()
        record['status'] = 'valid'
    except BaseException as error:
        record['error'] = str(error)
    finally:
        record.update(ended_utc=legacy.utc(), wall_seconds=time.monotonic()-reservation.began)
        legacy.save(reservation.output / 'run.json', record)
        reservation.finish()
    print(json.dumps(record))
    return int(record['status'] != 'valid')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--role', choices=('builder', 'reviewer'), required=True)
    sub = parser.add_subparsers(dest='command', required=True)
    build = sub.add_parser('build')
    build.add_argument('--ref', help='Explicit local future candidate; resolves once, labels output ad-hoc')
    for name in ('run', 'smoke'):
        p = sub.add_parser(name)
        p.add_argument('--wrk', required=True)
        p.add_argument('--output', required=True)
    for name in ('check', 'unit'):
        p = sub.add_parser(name)
        p.add_argument('--output', required=True)
    args = parser.parse_args()
    if args.command == 'build':
        root = identity.role_root(args.role)
        legacy.demand(not (root / 'refs.json').exists(), 'existing refs/builds: do not overwrite')
        commits = identity.configure(args.ref)
        root.mkdir(parents=True, exist_ok=True)
        legacy.save(root / 'refs.json', {'commits': commits, 'trees': {k: identity.legacy_build.TREES[k] for k in commits}, 'requested_ref': args.ref})
        for label in ('C','D'):
            identity.legacy_build.build(label, root / label)
        return 0
    return check(args) if args.command in ('check','unit') else run_suite(args)

if __name__ == '__main__':
    try:
        sys.exit(main())
    except (legacy.Invalid, ValueError, OSError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
