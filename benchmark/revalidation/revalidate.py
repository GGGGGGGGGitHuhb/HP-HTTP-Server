#!/usr/bin/env python3
"""Fixed C/D export, build and one-second HTTP smoke; no performance verdict."""
import argparse
import hashlib
import importlib.util
import io
import json
import math
import os
from pathlib import Path
import re
import resource
import shlex
import signal
import stat
import subprocess
import sys
import tarfile
import threading
import time

sys.dont_write_bytecode = True
REPOSITORY = Path(__file__).absolute().parents[2]
ROOT = REPOSITORY / '.cache/v0.5.1-revalidation'
SHARED_WORK_DEADLINE = None
SHARED_CLEANUP_DEADLINE = None
IDENTITIES = {
    'C': ('942f72cd9cea58e097025c3b9dd660f4132a1ffb', 'b407f052c7a974ae4fff4976c8275fb5905cf036'),
    'D': ('69424e6ab057bba2950c018e34c5695a4dc74f22', '826e20166caa334c95a1c6fdc957a128d5aee568'),
}
INPUTS = {'run.py': '9fb01e7b2da9b7d4c0f46546d1f92b3fd2506301fdb85c9febb00be436b081ba',
          'build.py': '32340a0c73946ad18938b0c48247a18f44eb8c297d495961a710fac8bea8e22b',
          'summary.lua': '0ded2fdca3ab1f53274ae70806117c3c6288fec1bb05b0a0b601d55bf36b73ea'}
FLAGS = ['-DCMAKE_BUILD_TYPE=Release', '-DBUILD_TESTING=OFF',
         '-DCMAKE_CXX_COMPILER=/usr/bin/g++', '-DCMAKE_CXX_STANDARD=20',
         '-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG', '-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF',
         '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON']
ALLOCATED_LIMIT = 2 * 1024 ** 3
LOG_LIMIT = 64 * 1024 ** 2


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    result = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1024 ** 2), b''):
            result.update(block)
    return result.hexdigest()


def save(path, value):
    with Path(path).open('x') as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write('\n')


def directories(path):
    """Reject symlink ancestors, including same-name roots outside this route."""
    path = Path(path).absolute()
    for candidate in reversed([path, *path.parents]):
        status = candidate.lstat()
        require(stat.S_ISDIR(status.st_mode), 'directory ancestor required: ' + str(candidate))
    return path


def role_root(role):
    require(role in ('builder', 'reviewer'), 'role rejected')
    return directories(ROOT / role)


def create_leaf(path, parent):
    parent = directories(parent)
    require(Path(path).absolute().parent == parent, 'output must have exact declared parent')
    Path(path).mkdir(exist_ok=False)
    return Path(path)


def measure(root):
    """One rule for runtime polling and final accounting; no symlink traversal."""
    directories(root)
    allocated = logical = logs = 0
    seen = set()
    vanished = []
    def visit(directory_fd, directory):
        nonlocal allocated, logical, logs
        allocated += os.fstat(directory_fd).st_blocks * 512
        with os.scandir(directory_fd) as entries:
            for entry in entries:
                try:
                    status = os.stat(entry.name, dir_fd=directory_fd, follow_symlinks=False)
                except FileNotFoundError:
                    vanished.append(str(directory / entry.name))
                    continue
                if stat.S_ISDIR(status.st_mode):
                    try:
                        child = os.open(entry.name, os.O_DIRECTORY | os.O_NOFOLLOW | os.O_RDONLY, dir_fd=directory_fd)
                    except FileNotFoundError:
                        vanished.append(str(directory / entry.name))
                        continue
                    try:
                        opened = os.fstat(child)
                        require((opened.st_dev, opened.st_ino) == (status.st_dev, status.st_ino), 'directory identity changed')
                        visit(child, directory / entry.name)
                    finally:
                        os.close(child)
                elif stat.S_ISREG(status.st_mode):
                    key = (status.st_dev, status.st_ino)
                    if key in seen:
                        continue
                    seen.add(key)
                    allocated += status.st_blocks * 512
                    logical += status.st_size
                    if entry.name.endswith(('.stdout', '.stderr', '.log')):
                        logs += status.st_size
                elif stat.S_ISLNK(status.st_mode):
                    # CMake's own generated aliases are counted as links, never followed.
                    allocated += status.st_blocks * 512
                    logical += status.st_size
                else:
                    raise ValueError('unsupported object in declared role tree: ' + str(directory / entry.name))
    root_fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        visit(root_fd, Path(root))
    finally:
        os.close(root_fd)
    require(allocated <= ALLOCATED_LIMIT, 'allocated tree limit exceeded')
    require(logs <= LOG_LIMIT, 'ordinary log logical limit exceeded')
    return {'allocated_bytes': allocated, 'logical_bytes': logical, 'log_logical_bytes': logs,
            'polling_is_not_hard_quota': True, 'vanished_during_scan': vanished}


def process_identity(pid):
    try:
        text = Path('/proc', str(pid), 'stat').read_text()
    except OSError as error:
        if error.errno in (2, 3):
            return None
        raise
    fields = text[text.rfind(')') + 2:].split()
    return {'pid': pid, 'starttime': int(fields[19]), 'ppid': int(fields[1]), 'pgid': int(fields[2])}


def same_process(expected):
    current = process_identity(expected['pid'])
    return current is not None and all(current[key] == expected[key] for key in ('pid', 'starttime', 'pgid'))


class Scope:
    def __init__(self, root, work_seconds, maximum_seconds):
        self.root = directories(root)
        self.started = time.monotonic()
        self.work_deadline = self.started + work_seconds
        self.cleanup_deadline = self.started + maximum_seconds - 1
        if SHARED_WORK_DEADLINE is not None:
            self.work_deadline = min(self.work_deadline, SHARED_WORK_DEADLINE)
            self.cleanup_deadline = min(self.cleanup_deadline, SHARED_CLEANUP_DEADLINE)
        self.children = []
        self.cleanup = []
        self.high_water = {'allocated_bytes': 0, 'logical_bytes': 0, 'log_logical_bytes': 0}
        self.monitor_error = None
        self.done = threading.Event()
        self.thread = threading.Thread(target=self.monitor, daemon=True)

    def poll(self):
        require(time.monotonic() < self.work_deadline, 'internal work deadline exceeded')
        if self.monitor_error is not None:
            raise self.monitor_error
        values = measure(self.root)
        for key in self.high_water:
            self.high_water[key] = max(self.high_water[key], values[key])
        return values

    def monitor(self):
        while not self.done.wait(.2):
            try:
                self.poll()
            except BaseException as error:
                self.monitor_error = error
                # Main thread receives TERM and enters finally; children share the supervised group.
                os.kill(os.getpid(), signal.SIGTERM)
                return

    def register(self, process):
        item = {'process': process, 'identity': None}
        self.children.append(item)  # Registration precedes fallible /proc acquisition.
        item['identity'] = process_identity(process.pid)
        require(item['identity'] is not None, 'child identity unavailable')
        require(item['identity']['pgid'] == os.getpgrp(), 'child escaped supervised process group')
        return item

    def close_child(self, item):
        process = item['process']
        errors = []
        forced = False
        descendants = []
        try:
            if process.poll() is None:
                remaining = self.cleanup_deadline - time.monotonic()
                require(remaining > 0, 'cleanup deadline exhausted')
                current = process_identity(process.pid)
                expected = item['identity']
                # Popen owns its unreaped direct child even when initial /proc read failed.
                require(expected is None or same_process(expected), 'child identity changed/unknown')
                parents = [process.pid]
                while parents:
                    parent = parents.pop()
                    try:
                        children = Path('/proc', str(parent), 'task', str(parent), 'children').read_text().split()
                    except OSError as error:
                        if error.errno in (2, 3):
                            continue
                        raise
                    for child in children:
                        identity = process_identity(int(child))
                        if identity is not None and identity['ppid'] == parent:
                            require(len(descendants) < 128, 'owned descendant bound exceeded')
                            descendants.append(identity)
                            parents.append(identity['pid'])
                process.terminate()
                try:
                    process.wait(timeout=min(2, remaining))
                except subprocess.TimeoutExpired:
                    forced = True
                    require(time.monotonic() < self.cleanup_deadline, 'cleanup deadline exhausted')
                    require(expected is None or same_process(expected), 'kill identity changed/unknown')
                    process.kill()
                    process.wait(timeout=max(.001, self.cleanup_deadline - time.monotonic()))
            else:
                process.wait(timeout=max(.001, self.cleanup_deadline - time.monotonic()))
            for owned in reversed(descendants):
                if same_process(owned):
                    forced = True
                    require(time.monotonic() < self.cleanup_deadline, 'descendant cleanup deadline exhausted')
                    os.kill(owned['pid'], signal.SIGKILL)
            while any(same_process(owned) for owned in descendants) and time.monotonic() < self.cleanup_deadline:
                time.sleep(.02)
            require(not any(same_process(owned) for owned in descendants), 'owned descendant remains')
        except BaseException as error:
            errors.append({'type': type(error).__name__, 'message': str(error)})
        result = {'pid': process.pid, 'identity': item['identity'], 'returncode': process.returncode,
                  'forced': forced, 'reaped': process.poll() is not None, 'errors': errors, 'descendants': descendants}
        result['remaining_descendants'] = []
        for owned in descendants:
            try:
                if same_process(owned):
                    result['remaining_descendants'].append(owned)
            except BaseException as error:
                result['remaining_descendants'].append(dict(owned, unknown=str(error)))
        self.cleanup.append(result)
        return result

    def finish(self):
        self.done.set()
        if self.thread.ident is not None:
            try:
                self.thread.join(timeout=.5)
                if self.thread.is_alive():
                    raise RuntimeError('monitor not stopped')
            except BaseException as error:
                self.cleanup.append({'pid': None, 'forced': False,
                                     'errors': [{'type': type(error).__name__}]})
        for item in reversed(self.children):
            try:
                if item['process'].poll() is None:
                    self.close_child(item)
            except BaseException as error:
                self.cleanup.append({'pid': item['process'].pid, 'forced': False,
                                     'errors': [{'type': type(error).__name__, 'message': str(error)}]})
        remaining = []
        for item in self.children:
            try:
                if item['process'].poll() is None:
                    remaining.append(item['process'].pid)
            except BaseException as error:
                remaining.append({'pid': item['process'].pid, 'unknown': type(error).__name__})
        remaining += [owned for row in self.cleanup for owned in row.get('remaining_descendants', [])]
        return {'children': self.cleanup, 'remaining': remaining,
                'complete': not remaining and not any(row['errors'] for row in self.cleanup)}

    def command(self, argv, stdout=None):
        self.poll()
        process = subprocess.Popen(argv, cwd=REPOSITORY, stdout=stdout or subprocess.PIPE,
                                   stderr=subprocess.STDOUT)
        item = self.register(process)
        try:
            while True:
                self.poll()
                try:
                    result = process.communicate(timeout=min(.1, max(.001, self.work_deadline - time.monotonic())))[0]
                    break
                except subprocess.TimeoutExpired:
                    continue
            require(process.returncode == 0, 'command failed: ' + repr(argv))
            return result or b''
        finally:
            self.close_child(item)


def load_legacy(scope):
    for name, expected in INPUTS.items():
        require(digest(REPOSITORY / 'benchmark' / name) == expected, 'readonly legacy input drift')
    def load(name, path):
        spec = importlib.util.spec_from_file_location(name, path)
        module = importlib.util.module_from_spec(spec)
        sys.modules[name] = module
        spec.loader.exec_module(module)
        return module
    old_build = sys.modules.get('build')
    try:
        load('build', REPOSITORY / 'benchmark/build.py')
        legacy = load('revalidation_legacy_run', REPOSITORY / 'benchmark/run.py')
    finally:
        if old_build is None:
            sys.modules.pop('build', None)
        else:
            sys.modules['build'] = old_build
    original = legacy.OwnedProcess
    class TrackedProcess(original):
        def __init__(self, command, prefix):
            self.command = command
            self.stdout_path, self.stderr_path = Path(str(prefix) + '.stdout'), Path(str(prefix) + '.stderr')
            self.stdout = self.stdout_path.open('xb')
            self.stderr = None
            self.item = None
            try:
                self.stderr = self.stderr_path.open('xb')
                self.process = subprocess.Popen(command, stdout=self.stdout, stderr=self.stderr)
                self.item = scope.register(self.process)
                self.identity = legacy.process_info(self.process.pid)
                self.forced = False
                self.stop_result = None
                legacy.save(str(prefix) + '.process.json', {'command': command, **self.identity})
            except BaseException:
                if self.item:
                    scope.close_child(self.item)
                self.stdout.close()
                if self.stderr:
                    self.stderr.close()
                raise

        def matches(self):
            current = process_identity(self.process.pid)
            return current is not None and current['starttime'] == self.identity['starttime']

        def close(self, timeout=7):
            result = scope.close_child(self.item)
            for stream in (self.stdout, self.stderr):
                try:
                    stream.close()
                except BaseException as error:
                    result['errors'].append({'type': type(error).__name__, 'message': str(error)})
            self.forced = result['forced'] or bool(result['errors']) or not result['reaped']
            self.stop_result = result['returncode']
            return dict(result, forced=self.forced, starttime=self.identity['starttime'])
    legacy.OwnedProcess = TrackedProcess
    legacy.log_guard = lambda output, limit=None: scope.poll()
    return legacy


def source_catalog(source):
    result = {}
    for path in sorted(Path(source).rglob('*')):
        status = path.lstat()
        if stat.S_ISREG(status.st_mode):
            result[str(path.relative_to(source))] = digest(path)
        else:
            require(stat.S_ISDIR(status.st_mode), 'source link or special file rejected')
    return result


def extract_archive(archive, source):
    source.mkdir()
    expected = {}
    with tarfile.open(archive) as stream:
        members = stream.getmembers()
        for member in members:
            path = Path(member.name)
            require(not path.is_absolute() and '..' not in path.parts, 'unsafe archive path')
            require(member.isdir() or member.isfile(), 'archive link/special entry rejected')
            require(member.name not in expected, 'duplicate archive entry')
            expected[member.name] = {'type': 'directory' if member.isdir() else 'file', 'bytes': member.size}
        stream.extractall(source, filter='data')
    return expected


def libraries(scope, binary):
    output = scope.command(['/usr/bin/ldd', str(binary)]).decode()
    require('not found' not in output, 'dynamic dependency missing')
    files = {}
    for line in output.splitlines():
        match = re.search(r'(?:=>\s+)?(/\S+)\s+\(', line)
        if match:
            files[match.group(1)] = digest(match.group(1))
    require(bool(files), 'dynamic dependency catalog missing')
    return {'ldd': output, 'files': files}


def compiled_dependencies(build_root):
    files = {}
    dependency_files = list(Path(build_root).rglob('*.o.d'))
    require(bool(dependency_files), 'actual compiler dependency files missing')
    for path in dependency_files:
        text = path.read_text().replace('\\\n', ' ')
        require(': ' in text, 'compiler dependency record malformed')
        for token in shlex.split(text.split(': ', 1)[1]):
            dependency = Path(token)
            if not dependency.is_absolute():
                dependency = Path(build_root) / dependency
            require(dependency.is_file(), 'actual compiled dependency missing')
            files[str(dependency.absolute())] = digest(dependency)
    return files


def validate_manifest(manifest, role, label, scope):
    expected = role_root(role) / label
    commit, tree = IDENTITIES[label]
    require(manifest.get('label') == label and manifest.get('commit') == commit and manifest.get('tree') == tree,
            'fixed commit/tree mismatch')
    require(manifest['source'] == str(expected / 'source') and manifest['archive'] == str(expected / 'source.tar'), 'manifest path ownership mismatch')
    require(manifest['flags'] == FLAGS, 'fixed compile flags mismatch')
    require(digest(manifest['archive']) == manifest['archive_sha256'], 'archive changed')
    with tarfile.open(manifest['archive']) as archive:
        archive_sources = {entry.name: hashlib.sha256(archive.extractfile(entry).read()).hexdigest()
                           for entry in archive if entry.isfile()}
    require(archive_sources == manifest['source_hashes'], 'archive/source map mismatch')
    require(source_catalog(manifest['source']) == manifest['source_hashes'], 'source changed')
    for key, suffix in (('binary', 'build/hp_http_server'), ('cmake_cache', 'build/CMakeCache.txt'), ('compile_commands', 'build/compile_commands.json')):
        require(manifest[key] == str(expected / suffix), 'manifest artifact path mismatch')
        require(digest(manifest[key]) == manifest[key + '_sha256'], key + ' changed')
    # communicate drains this pipe while the command is alive; it cannot fill and deadlock.
    actual_archive = scope.command(['/usr/bin/git', 'archive', '--format=tar', commit])
    require(hashlib.sha256(actual_archive).hexdigest() == manifest['archive_sha256'], 'fixed commit/archive mismatch')
    commands = json.loads(Path(manifest['compile_commands']).read_text())
    require(bool(commands), 'empty compile commands')
    for item in commands:
        command = item['command']
        require(all(flag in command for flag in ('/usr/bin/g++', '-O3', '-DNDEBUG', '-std=c++20')), 'actual compilation configuration mismatch')
        require(not any(flag in command for flag in ('-fsanitize', '-flto', '-march', '-mtune')), 'forbidden compilation flag')
    require(scope.command(['/usr/bin/git', 'rev-parse', commit + '^{tree}']).decode().strip() == tree, 'actual git tree mismatch')
    require(libraries(scope, manifest['binary'])['files'] == manifest['libraries']['files'], 'actual dynamic dependency drift')
    for path, expected_hash in manifest['tools'].items():
        require(digest(path) == expected_hash, 'build tool changed')
    require(compiled_dependencies(expected / 'build') == manifest['compiled_dependencies'], 'compiled dependency drift')
    return manifest


def build_version(scope, role, label):
    root = role_root(role)
    output = create_leaf(root / label, root)
    commit, tree = IDENTITIES[label]
    require(scope.command(['/usr/bin/git', 'rev-parse', commit + '^{tree}']).decode().strip() == tree, 'fixed git commit/tree mismatch')
    archive = output / 'source.tar'
    with archive.open('xb') as stream:
        scope.command(['/usr/bin/git', 'archive', '--format=tar', commit], stream)
    entries = extract_archive(archive, output / 'source')
    configure = ['/usr/bin/cmake', '-S', str(output / 'source'), '-B', str(output / 'build'), *FLAGS]
    compile_command = ['/usr/bin/cmake', '--build', str(output / 'build'), '--target', 'hp_http_server', '-j2']
    for filename, command in (('configure.log', configure), ('build.log', compile_command)):
        with (output / filename).open('x') as stream:
            scope.command(command, stream)
    manifest = {'schema': 'revalidation-build-v1', 'role': role, 'label': label, 'commit': commit, 'tree': tree,
                'archive': str(archive), 'archive_sha256': digest(archive), 'archive_entries': entries,
                'source': str(output / 'source'), 'source_hashes': source_catalog(output / 'source'),
                'flags': FLAGS, 'configure_command': configure, 'build_command': compile_command,
                'tools': {path: digest(path) for path in ('/usr/bin/g++', '/usr/bin/cmake', '/usr/bin/git', '/usr/bin/make', '/usr/bin/ld', '/usr/bin/as', '/usr/bin/ar', '/usr/bin/ranlib')},
                'versions': {path: scope.command([path, '--version']).decode().splitlines()[0] for path in ('/usr/bin/g++', '/usr/bin/cmake', '/usr/bin/git', '/usr/bin/make')}}
    for key, suffix in (('binary', 'build/hp_http_server'), ('cmake_cache', 'build/CMakeCache.txt'), ('compile_commands', 'build/compile_commands.json')):
        manifest[key] = str(output / suffix)
        manifest[key + '_sha256'] = digest(output / suffix)
    manifest['libraries'] = libraries(scope, manifest['binary'])
    manifest['compiled_dependencies'] = compiled_dependencies(output / 'build')
    validate_manifest(manifest, role, label, scope)
    save(output / 'manifest.json', manifest)
    return {'manifest': str(output / 'manifest.json'), 'manifest_sha256': digest(output / 'manifest.json')}


def smoke_version(scope, role, label, output):
    root = role_root(role)
    require(Path(output).absolute() == root / ('smoke-' + label + '-001'), 'exact smoke output required')
    create_leaf(output, root)
    output = Path(output)
    manifest_path = root / label / 'manifest.json'
    manifest = validate_manifest(json.loads(manifest_path.read_text()), role, label, scope)
    legacy = load_legacy(scope)
    tool = REPOSITORY / '.cache/v0.5-s4/tools/root/usr/bin/wrk'
    require(digest(tool) == legacy.WRK_SHA256 and os.access(tool, os.X_OK), 'fixed wrk binary rejected')
    # This package prints version/usage with exit1; preserve its actual status instead of demanding exit0.
    with (output / 'wrk-version.log').open('x') as stream:
        process = subprocess.Popen([str(tool), '--version'], stdout=stream, stderr=subprocess.STDOUT)
        item = scope.register(process)
        try:
            process.wait(timeout=max(.001, min(5, scope.work_deadline - time.monotonic())))
            require('wrk debian/4.1.0-4build2' in (output / 'wrk-version.log').read_text(), 'wrk version mismatch')
            require(process.returncode == 1, 'wrk version exit differs from official package')
        finally:
            scope.close_child(item)
    tool_libraries = libraries(scope, tool)
    save(output / 'tool.json', {'binary': str(tool), 'sha256': digest(tool), 'version_exit': process.returncode, 'libraries': tool_libraries})
    document = output / 'document'
    document.mkdir()
    payload = legacy.fixture(document, 1024)
    fixture_path = document / payload['name']
    primary_error = None
    try:
        sample = legacy.run_sample(manifest, tool, document, payload, output / 'sample', output,
                                   scope.work_deadline, warmup=1, duration=1)
        validate_manifest(manifest, role, label, scope)
        require(digest(tool) == legacy.WRK_SHA256 and libraries(scope, tool)['files'] == tool_libraries['files'], 'wrk/dependency changed during sample')
        for name, expected_hash in INPUTS.items():
            require(digest(REPOSITORY / 'benchmark' / name) == expected_hash, 'legacy input changed during sample')
        require(sample['status'] == 'valid' and sample['cleanup']['reaped'] and not sample['cleanup']['forced'], 'smoke cleanup invalid')
        return {'sample': str(output / 'sample/sample.json'), 'sample_sha256': digest(output / 'sample/sample.json'),
                'manifest_sha256': digest(manifest_path), 'no_performance_verdict': True}
    except BaseException as error:
        primary_error = error
        raise
    finally:
        cleanup_errors = []
        for operation in (fixture_path.unlink, document.rmdir):
            try:
                operation()
            except BaseException as error:
                cleanup_errors.append({'type': type(error).__name__, 'message': str(error)})
        if cleanup_errors:
            try:
                save(output / 'fixture-cleanup-errors.json', {'first_error': str(primary_error) if primary_error else None,
                                                             'cleanup_errors': cleanup_errors})
            except BaseException as evidence_error:
                cleanup_errors.append({'type': type(evidence_error).__name__, 'message': str(evidence_error)})
                try:
                    print('fixture cleanup evidence failed', file=sys.stderr)
                except BaseException:
                    pass  # Original cleanup failure still forces invalid below.
            if primary_error is not None:
                try:
                    primary_error.add_note('fixture cleanup errors: ' + repr(cleanup_errors))
                except BaseException:
                    pass
            else:
                raise ValueError('fixture cleanup failed: ' + repr(cleanup_errors))


def preflight(root):
    memory = dict((line.split(':')[0], int(line.split()[1])) for line in Path('/proc/meminfo').read_text().splitlines())
    import shutil
    free = shutil.disk_usage(root).free
    limit = resource.getrlimit(resource.RLIMIT_NOFILE)[0]
    require(memory['MemAvailable'] >= 1048576, 'MemAvailable below1GiB')
    require(free >= 4 * 1024 ** 3, 'disk free below4GiB')
    require(limit == resource.RLIM_INFINITY or limit >= 256, 'nofile below256')
    return {'MemAvailable_kib': memory['MemAvailable'], 'disk_free_bytes': free, 'nofile': limit}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--role', choices=('builder', 'reviewer'), required=True)
    parser.add_argument('--shared-work-deadline-ns', type=int)
    parser.add_argument('--shared-cleanup-deadline-ns', type=int)
    commands = parser.add_subparsers(dest='action', required=True)
    for action in ('build', 'smoke'):
        command = commands.add_parser(action)
        command.add_argument('--label', choices=IDENTITIES, required=True)
        if action == 'smoke':
            command.add_argument('--output', required=True)
    args = parser.parse_args(argv)
    root = role_root(args.role)
    step = args.action + '-' + args.label
    require(not (root / 'control' / (step + '.result.json')).exists(), 'step already attempted')
    maximum = 240 if args.action == 'build' else 45
    global SHARED_WORK_DEADLINE, SHARED_CLEANUP_DEADLINE
    if args.shared_work_deadline_ns is not None or args.shared_cleanup_deadline_ns is not None:
        require(args.shared_work_deadline_ns is not None and args.shared_cleanup_deadline_ns is not None, 'both common deadlines required')
        SHARED_WORK_DEADLINE = args.shared_work_deadline_ns / 1e9
        # Leave two seconds for the independent executor's post-wait evidence.
        SHARED_CLEANUP_DEADLINE = args.shared_cleanup_deadline_ns / 1e9 - 2
        require(time.monotonic() < SHARED_WORK_DEADLINE < SHARED_CLEANUP_DEADLINE, 'common deadline exhausted/invalid')
    scope = Scope(root, maximum - 10, maximum)
    previous = signal.getsignal(signal.SIGTERM)
    def interrupted(signum, frame):
        raise TimeoutError('TERM received; bounded finally cleanup')
    signal.signal(signal.SIGTERM, interrupted)
    failure = None
    result = None
    cleanup = None
    resources = None
    late_errors = []
    def preserve(error):
        nonlocal failure
        try:
            message = str(error)
        except BaseException:
            message = 'error message unavailable'
        value = {'type': type(error).__name__, 'message': message}
        if failure is None:
            failure = value
        else:
            late_errors.append(value)
    try:
        # Resource probes, imports and process checks all occur inside this charged invocation.
        resources = preflight(root)
        scope.poll()
        scope.thread.start()
        result = build_version(scope, args.role, args.label) if args.action == 'build' else smoke_version(scope, args.role, args.label, args.output)
    except BaseException as error:
        preserve(error)
    finally:
        # A second TERM cannot replace the first failure while bounded cleanup runs.
        try:
            signal.signal(signal.SIGTERM, signal.SIG_IGN)
        except BaseException as error:
            preserve(error)
        try:
            cleanup = scope.finish()
        except BaseException as error:
            preserve(error)
            cleanup = {'complete': False, 'remaining': ['cleanup_unknown'], 'children': scope.cleanup}
        try:
            final_bytes = measure(root)
            require(not final_bytes['vanished_during_scan'], 'final accounting did not obtain a stable tree')
        except BaseException as error:
            final_bytes = {'error': type(error).__name__}
            preserve(error)
        valid = failure is None and cleanup['complete'] and not cleanup['remaining'] and all(not row['forced'] and not row['errors'] for row in cleanup['children'])
        record = {'status': 'valid' if valid else 'invalid', 'error': failure,
             'result': result, 'cleanup': cleanup, 'final_bytes': final_bytes, 'high_water': scope.high_water,
             'late_errors': late_errors, 'shared_work_deadline_ns': args.shared_work_deadline_ns,
             'shared_cleanup_deadline_ns': args.shared_cleanup_deadline_ns,
             'resources': resources, 'environment': {key: os.environ.get(key) for key in ('TMPDIR', 'TMP', 'TEMP', 'XDG_CACHE_HOME', 'PYTHONDONTWRITEBYTECODE', 'NO_PROXY', 'LD_LIBRARY_PATH')},
             'elapsed_internal_seconds': time.monotonic() - scope.started,
             'time_scope': 'internal only; external time wall required for settlement', 'pgid': os.getpgrp()}
        try:
            save(root / 'control' / (step + '.result.json'), record)
        except BaseException as error:
            preserve(error)
            valid = False
            try:
                print('result evidence write failed; step remains unknown', file=sys.stderr)
            except BaseException as second:
                preserve(second)
        try:
            signal.signal(signal.SIGTERM, previous)
        except BaseException as error:
            preserve(error)
            valid = False
    require(valid, 'revalidation step invalid; all later steps must stop')


if __name__ == '__main__':
    main()
