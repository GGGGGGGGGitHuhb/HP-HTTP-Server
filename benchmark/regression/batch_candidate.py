#!/usr/bin/env python3
"""Approved R005: immutable D + scoped patch, explicit supplemental budget, C/E runs."""
import argparse
import copy
import hashlib
import json
import math
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import tarfile
import time

import budget
import executor
import identity
import model
import supervision
from identity import legacy

AUTH_ROOT = identity.REPO / '.cache/v0.5.1-s3/leader/r005'
EXCEPTIONS = ('CMakeLists.txt', 'include/base/AsyncLogger.h', 'src/base/AsyncLogger.cpp')
NEW_TEST = 'tests/AsyncLoggerBatch_test.cpp'
CLOCKS = (time.CLOCK_REALTIME, time.CLOCK_MONOTONIC, time.CLOCK_BOOTTIME)


def read_json(path):
    return json.loads(pathlib.Path(path).read_text())


def clocks():
    return [time.clock_gettime(clock) for clock in CLOCKS]


def elapsed(first):
    spans = [now - then for now, then in zip(clocks(), first)]
    legacy.demand(all(math.isfinite(span) and span >= 0 for span in spans), 'invalid/backward clock')
    legacy.demand(max(spans) - min(spans) <= 3, 'clock disagreement')
    return max(spans)


def authorization(role):
    data = read_json(AUTH_ROOT / 'authorization.json')
    legacy.demand(data.get('schema') == 1 and data.get('approved_date') == '2026-10-05', 'R005 approval missing')
    legacy.demand(data['additional_seconds_per_role'] == 1800 and data['additional_log_bytes_per_role'] == 2 * 1024**3,
                  'unauthorized supplemental limits')
    baseline = data['baseline'][role]
    budget.number(baseline['charged_seconds'])
    budget.number(baseline['log_bytes'])
    legacy.demand('状态：Approved' in (identity.REPO / 'docs/leader/reworks/V0.5.1/S3-rework-005.md').read_text(),
                  'R005 is not Approved')
    return data


def check_protection():
    protected = read_json(AUTH_ROOT / 'protected-before.json')
    changed = [name for name, digest in protected.items()
               if name not in EXCEPTIONS and legacy.sha(identity.REPO / name) != digest]
    legacy.demand(not changed, 'protected file changed: ' + ', '.join(changed))
    return {'protected_total': len(protected), 'exceptions': list(EXCEPTIONS), 'unchanged': len(protected) - len(EXCEPTIONS)}


def frozen():
    seal = read_json(AUTH_ROOT / 'seal.json')
    descriptor = AUTH_ROOT / 'candidate.json'
    legacy.demand(legacy.sha(descriptor) == seal['descriptor_sha256'], 'candidate descriptor drift')
    data = read_json(descriptor)
    legacy.demand(data['base_commit'] == identity.COMMITS['D'] and data['base_tree'] == identity.TREES['D'], 'candidate base drift')
    legacy.demand(legacy.sha(AUTH_ROOT / 'candidate.patch') == data['patch_sha256'], 'candidate patch drift')
    legacy.demand(set(data['changed_files']) == set(EXCEPTIONS) | {NEW_TEST}, 'candidate scope drift')
    return data, seal


def freeze():
    authorization('builder')
    legacy.demand(not (AUTH_ROOT / 'seal.json').exists(), 'candidate already frozen; do not overwrite')
    check_protection()
    patch = subprocess.check_output(['git', 'diff', '--binary', identity.COMMITS['D'], '--', *EXCEPTIONS], cwd=identity.REPO)
    new_patch = subprocess.run(['git', 'diff', '--no-index', '--binary', '--', '/dev/null', NEW_TEST],
                               cwd=identity.REPO, stdout=subprocess.PIPE, check=False)
    legacy.demand(new_patch.returncode == 1, 'new test patch missing')
    patch += new_patch.stdout
    patch_path = AUTH_ROOT / 'candidate.patch'
    patch_path.write_bytes(patch)
    paths = subprocess.check_output(['git', 'apply', '--numstat', str(patch_path)], cwd=identity.REPO, text=True)
    changed = [line.split('\t')[-1] for line in paths.splitlines()]
    legacy.demand(set(changed) == set(EXCEPTIONS) | {NEW_TEST}, 'patch touches unapproved files')
    descriptor = {'schema': 1, 'label': 'E', 'base_commit': identity.COMMITS['D'], 'base_tree': identity.TREES['D'],
                  'patch_sha256': legacy.sha(patch_path), 'changed_files': changed,
                  'changed_hashes': {name: legacy.sha(identity.REPO / name) for name in changed}}
    legacy.save(AUTH_ROOT / 'candidate.json', descriptor)
    legacy.save(AUTH_ROOT / 'seal.json', {'descriptor_sha256': legacy.sha(AUTH_ROOT / 'candidate.json')})
    print(json.dumps({'candidate': 'E', 'base_commit': descriptor['base_commit'], 'patch_sha256': descriptor['patch_sha256']}))


def candidate_root(role):
    return identity.role_root(role) / 'E-r005'


def logged_command(command, path):
    began = time.monotonic()
    with pathlib.Path(path).open('w') as stream:
        subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=True)
    return {'command': command, 'wall_seconds': time.monotonic() - began}


def build(role, mode):
    authorization(role)
    descriptor, seal = frozen()
    check_protection()
    directory = candidate_root(role)
    source = directory / 'source'
    directory.mkdir(exist_ok=True)
    if mode == 'release':
        legacy.demand(not source.exists(), 'candidate export already exists')
        archive = directory / 'base-source.tar'
        archive.write_bytes(identity.legacy_build.archive_bytes('D'))
        source.mkdir()
        with tarfile.open(archive) as stream:
            stream.extractall(source, filter='data')
        patch_environment = {**os.environ, 'GIT_CEILING_DIRECTORIES': str(directory)}
        subprocess.run(['git', 'apply', '--check', str(AUTH_ROOT / 'candidate.patch')], cwd=source, env=patch_environment, check=True)
        subprocess.run(['git', 'apply', str(AUTH_ROOT / 'candidate.patch')], cwd=source, env=patch_environment, check=True)
        hashes = {str(path.relative_to(source)): legacy.sha(path) for path in sorted(source.rglob('*')) if path.is_file()}
        for name, digest in descriptor['changed_hashes'].items():
            legacy.demand(hashes[name] == digest, 'export patch differs from approved candidate')
        flags = identity.legacy_build.FLAGS
        target = 'hp_http_server'
        binary_dir = directory / 'build-release'
    else:
        validate_candidate(role)
        binary_dir = source / ('build-' + mode)
        flags = ['-DCMAKE_BUILD_TYPE=Debug', '-DBUILD_TESTING=ON', '-DCMAKE_CXX_COMPILER=/usr/bin/g++']
        target = 'all' if mode == 'debug' else 'async_logger_batch_tests'
        if mode in ('asan', 'tsan'):
            sanitizers = 'address,undefined' if mode == 'asan' else 'thread'
            flags += ['-DCMAKE_CXX_FLAGS=-fsanitize=' + sanitizers + ' -fno-omit-frame-pointer -fno-pie',
                      '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=' + sanitizers + ' -no-pie']
    legacy.demand(not binary_dir.exists(), 'build directory already exists; do not overwrite')
    compile_record = [logged_command(['cmake', '-S', str(source), '-B', str(binary_dir), *flags], directory / (mode + '-configure.log')),
                      logged_command(['cmake', '--build', str(binary_dir), '--target', target, '-j4'], directory / (mode + '-build.log'))]
    legacy.save(directory / (mode + '-build-record.json'), compile_record)
    if mode == 'release':
        manifest = {'schema': 1, 'label': 'E', 'commit': descriptor['base_commit'], 'tree': descriptor['base_tree'],
                    'candidate_descriptor_sha256': seal['descriptor_sha256'], 'patch_sha256': descriptor['patch_sha256'],
                    'archive': str(archive), 'archive_sha256': legacy.sha(archive), 'source': str(source), 'source_hashes': hashes,
                    'binary': str(binary_dir / 'hp_http_server'), 'flags': flags, 'build_record': compile_record}
        for key, name in (('binary', 'hp_http_server'), ('cmake_cache', 'CMakeCache.txt'), ('compile_commands', 'compile_commands.json')):
            manifest[key] = str(binary_dir / name)
            manifest[key + '_sha256'] = legacy.sha(binary_dir / name)
        legacy.save(directory / 'manifest.json', manifest)
    print(json.dumps({'role': role, 'build': mode, 'status': 'built', 'seconds': sum(item['wall_seconds'] for item in compile_record)}))


def validate_candidate(role):
    descriptor, seal = frozen()
    directory = candidate_root(role)
    manifest = read_json(directory / 'manifest.json')
    source = directory / 'source'
    legacy.demand(manifest['label'] == 'E' and manifest['commit'] == descriptor['base_commit'] and manifest['tree'] == descriptor['base_tree'], 'candidate identity mismatch')
    legacy.demand(manifest['source'] == str(source) and manifest['binary'] == str(directory / 'build-release/hp_http_server'), 'candidate not owned by role')
    legacy.demand(manifest['candidate_descriptor_sha256'] == seal['descriptor_sha256'] and manifest['patch_sha256'] == descriptor['patch_sha256'], 'candidate patch mismatch')
    legacy.demand(manifest['flags'] == identity.legacy_build.FLAGS, 'candidate Release flags mismatch')
    expected_archive = identity.legacy_build.archive_bytes('D')
    legacy.demand(manifest['archive_sha256'] == hashlib.sha256(expected_archive).hexdigest() and legacy.sha(manifest['archive']) == manifest['archive_sha256'], 'base archive mismatch')
    with tarfile.open(manifest['archive']) as stream:
        expected = {member.name: hashlib.sha256(stream.extractfile(member).read()).hexdigest() for member in stream if member.isfile()}
    expected.update(descriptor['changed_hashes'])
    legacy.demand(manifest['source_hashes'] == expected, 'candidate complete source map mismatch')
    for name, digest in expected.items():
        legacy.demand(legacy.sha(source / name) == digest, 'candidate source drift: ' + name)
    for key in ('binary', 'cmake_cache', 'compile_commands'):
        legacy.demand(legacy.sha(manifest[key]) == manifest[key + '_sha256'], 'candidate ' + key + ' drift')
    for item in read_json(manifest['compile_commands']):
        command = item['command']
        compiled_source = pathlib.Path(item['file'])
        legacy.demand(compiled_source.is_relative_to(source) and str(compiled_source.relative_to(source)) in expected,
                      'compiled source outside frozen candidate')
        legacy.demand(all(flag in command for flag in ('-O3', '-DNDEBUG', '-std=c++20')), 'candidate compile flags mismatch')
        legacy.demand(not any(flag in command for flag in ('-fsanitize', '-flto', '-march', '-mtune')), 'noncomparable candidate flags')
    return manifest


def snapshot(role, wrk):
    root = identity.role_root(role)
    check_protection()
    baseline = legacy.validate_manifest(root / 'C/manifest.json', 'C')
    legacy.demand(baseline['source'] == str(root / 'C/source') and baseline['binary'] == str(root / 'C/build/hp_http_server'), 'baseline not owned by role')
    candidate = validate_candidate(role)
    libraries = {}
    for label, manifest in (('C', baseline), ('E', candidate)):
        import re
        listing = subprocess.check_output(['ldd', manifest['binary']], text=True)
        legacy.demand('not found' not in listing, 'server dependency missing')
        libraries[label] = {match.group(1): legacy.sha(match.group(1)) for line in listing.splitlines()
                            if (match := re.search(r'(?:=>\s+)?(/\S+)\s+\(', line))}
    return {'manifests': {'C': baseline, 'E': candidate}, 'wrk': identity.stable_tool(legacy.validate_tool(wrk)),
            'scripts': identity.scripts(), 'compiler_sha256': legacy.sha('/usr/bin/g++'), 'server_libraries': libraries,
            'authorization_sha256': legacy.sha(AUTH_ROOT / 'authorization.json')}


def schedule(smoke=False):
    result = copy.deepcopy(model.schedule(smoke))
    for item in result:
        if item['label'] == 'D':
            item['label'] = 'E'
    return result


def aggregate(rows):
    legacy.demand([row['schedule'] for row in rows] == schedule(), 'C/E schedule mismatch')
    normalized = copy.deepcopy(rows)
    for row in normalized:
        if row['label'] == 'E':
            row['label'] = 'D'
            row['schedule']['label'] = 'D'
    result = model.aggregate(normalized)
    result['groups'] = {(key[:-1] + 'E' if key.endswith('D') else key): value for key, value in result['groups'].items()}
    result['failures'] = [entry.replace('D:', 'E:') for entry in result['failures']]
    result['ratio_definition'] = 'E/C; original thresholds unchanged'
    return result


class Supplement:
    def __init__(self, role, output, kind, allowance):
        self.role = role
        self.root = identity.role_root(role)
        self.auth = authorization(role)
        self.baseline = self.auth['baseline'][role]
        self.first = clocks()
        budget.TOTAL_SECONDS = self.baseline['charged_seconds'] + self.auth['additional_seconds_per_role']
        self.reservation = budget.Reservation(self.root, output, kind, allowance)
        legacy.demand(self.reservation.previous >= self.baseline['charged_seconds'], 'historical budget rollback')

    def guard(self, root=None, limit=None):
        legacy.demand(root is None or pathlib.Path(root).resolve() == self.root, 'log guard role mismatch')
        legacy.demand(elapsed(self.first) <= self.reservation.allowance, 'supplement watchdog')
        size = legacy.log_bytes(self.root)
        legacy.demand(size >= self.baseline['log_bytes'], 'historical logs removed')
        legacy.demand(size - self.baseline['log_bytes'] <= self.auth['additional_log_bytes_per_role'], 'supplement log budget exhausted')
        return size

    def finish(self, record):
        reservation = self.reservation
        try:
            seconds = max(elapsed(self.first), time.monotonic() - reservation.began)
            self.guard()
        except BaseException as error:
            record['status'] = 'invalid'
            record['final_guard_error'] = str(error)
            seconds = max(reservation.allowance, time.monotonic() - reservation.began)
        record.update(ended_utc=legacy.utc(), wall_seconds=seconds, role_log_bytes=legacy.log_bytes(self.root),
                      prior_dynamic_seconds=reservation.previous, supplementary_authority='R005',
                      supplemental_seconds_used=reservation.previous + seconds - self.baseline['charged_seconds'],
                      supplemental_log_bytes=legacy.log_bytes(self.root) - self.baseline['log_bytes'])
        if record['status'] == 'invalid':
            record.pop('performance_acceptance', None)
        reservation.entry.update(state='finished', charged_seconds=seconds, ended_utc=record['ended_utc'], authority='R005')
        budget.atomic(reservation.path, reservation.data)
        reservation.lock.close()
        legacy.save(reservation.output / 'run.json', record)


def interrupted(signum, frame):
    raise KeyboardInterrupt('signal ' + str(signum))


def run(args):
    root = identity.role_root(args.role)
    auth = authorization(args.role)
    kind = 'r005-' + args.command
    old = budget.load(root / 'ledger.json')
    if args.command in ('smoke', 'matrix'):
        legacy.demand(not any(entry['kind'] == kind for entry in old['entries'].values()), 'R005 run already attempted; no retry')
    allowance = 60 if args.command == 'smoke' else 900 if args.command == 'matrix' else 180
    supplement = Supplement(args.role, args.output, kind, allowance)
    record = {'schema': 1, 'run_id': supplement.reservation.id, 'kind': kind, 'status': 'invalid',
              'started_utc': legacy.utc(), 'samples': [], 'authorization': auth, 'candidate_label': 'E'}
    owned = []
    original_owned = executor.OwnedProcess
    original_guard = legacy.log_guard
    original_executor_guard = executor.log_guard
    previous_handlers = {sig: signal.getsignal(sig) for sig in (signal.SIGINT, signal.SIGTERM)}

    class OwnedCandidateProcess(original_owned):
        def __init__(self, command, prefix):
            super().__init__(command, prefix)
            owned.append(self)

    fixture_root = supplement.reservation.output / 'root'
    try:
        legacy.log_guard = supplement.guard
        executor.log_guard = supplement.guard
        executor.OwnedProcess = OwnedCandidateProcess
        for sig in previous_handlers:
            signal.signal(sig, interrupted)
        supplement.guard()
        record['preflight'] = supervision.resources(root)
        record['environment'] = legacy.environment(root)
        if args.command == 'check':
            validate_candidate(args.role)
            record['command'] = args.check_command
            legacy.demand(bool(args.check_command), 'missing check command')
            record['cleanup'] = supervision.run(args.check_command, root, supplement.reservation.output / 'checks')
            record['status'] = 'valid'
        else:
            before = snapshot(args.role, args.wrk)
            record['identity_before'] = before
            record['schedule'] = schedule(args.command == 'smoke')
            legacy.save(supplement.reservation.output / 'schedule.json', record['schedule'])
            legacy.save(supplement.reservation.output / 'run.json', record)
            fixture_root.mkdir()
            fixtures = {size: legacy.fixture(fixture_root, size) for size in {item['size'] for item in record['schedule']}}
            deadline = supplement.reservation.began + supplement.reservation.allowance
            for index, item in enumerate(record['schedule'], 1):
                supplement.guard()
                row = executor.run_sample(before['manifests'][item['label']], args.wrk, fixture_root, fixtures[item['size']],
                                          supplement.reservation.output / f"{index:02d}-{item['scenario']}-{item['label']}",
                                          root, deadline, item, 1 if args.command == 'smoke' else 5, 1 if args.command == 'smoke' else 20)
                record['samples'].append(row)
                legacy.save(supplement.reservation.output / 'run.json', record)
            record['identity_after'] = snapshot(args.role, args.wrk)
            legacy.demand(before == record['identity_after'], 'identity drift')
            if args.command == 'matrix':
                observed = aggregate(record['samples'])
                record['observed_comparison'] = observed
                record['performance_acceptance'] = 'FAIL' if observed['failures'] else 'PASS'
            else:
                record['performance_acceptance'] = 'NOT_APPLICABLE_SMOKE'
            record['status'] = 'valid'
    except BaseException as error:
        record['error'] = type(error).__name__ + ': ' + str(error)
    finally:
        for sig in previous_handlers:
            signal.signal(sig, signal.SIG_IGN)
        cleanups = []
        for child in owned:
            try:
                result = child.close()
                cleanups.append(result)
                if result['forced'] or not result['reaped']:
                    record['status'] = 'invalid'
            except BaseException as error:
                cleanups.append({'error': str(error)})
                record['status'] = 'invalid'
        record['owned_final_cleanup'] = cleanups
        if fixture_root.exists():
            shutil.rmtree(fixture_root)
        supplement.finish(record)
        legacy.log_guard = original_guard
        executor.log_guard = original_executor_guard
        executor.OwnedProcess = original_owned
        for sig, handler in previous_handlers.items():
            signal.signal(sig, handler)
    print(json.dumps({'status': record['status'], 'performance_acceptance': record.get('performance_acceptance'),
                      'samples': len(record['samples']), 'error': record.get('error'), 'final_guard_error': record.get('final_guard_error'),
                      'supplemental_seconds_used': record['supplemental_seconds_used']}), flush=True)
    return int(record['status'] != 'valid' or record.get('performance_acceptance') == 'FAIL')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--role', choices=('builder', 'reviewer'), default='builder')
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('freeze')
    build_parser = sub.add_parser('build')
    build_parser.add_argument('--mode', choices=('release', 'debug', 'asan', 'tsan'), default='release')
    for name in ('smoke', 'matrix', 'check'):
        child = sub.add_parser(name)
        child.add_argument('--output', required=True)
        if name == 'check':
            child.add_argument('check_command', nargs=argparse.REMAINDER)
        else:
            child.add_argument('--wrk', required=True)
    args = parser.parse_args()
    if args.command == 'freeze':
        freeze()
        return 0
    if args.command == 'build':
        build(args.role, args.mode)
        return 0
    if args.command == 'check' and args.check_command[:1] == ['--']:
        args.check_command = args.check_command[1:]
    return run(args)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (legacy.Invalid, OSError, ValueError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
