#!/usr/bin/env python3
"""R029: one frozen row, acknowledged timeout group, same-parent final observation."""
import argparse
import errno
import hashlib
import json
import math
import os
from pathlib import Path
import resource
import re
import select
import signal
import stat
import subprocess
import sys
import time

sys.dont_write_bytecode = True
REPOSITORY = Path(__file__).absolute().parents[2]
STAGE = REPOSITORY / '.cache/v0.5.1-revalidation'
MAX_JSON = 4 * 1024 ** 2
MAX_PROC_ENTRIES = 65536
OLD_INPUTS = {
    'benchmark/revalidation/revalidate.py': '23dde7da91b09ef6cc183dce99f849d29804c6f22557ecd4d8e9974ed26ea0f5',
    'benchmark/revalidation/test_revalidate.py': '846565d2ee61e03da1a2410ed221dae44ea4bda4a0f4fd2598b0371295148760',
    'benchmark/run.py': '9fb01e7b2da9b7d4c0f46546d1f92b3fd2506301fdb85c9febb00be436b081ba',
    'benchmark/build.py': '32340a0c73946ad18938b0c48247a18f44eb8c297d495961a710fac8bea8e22b',
    'benchmark/summary.lua': '0ded2fdca3ab1f53274ae70806117c3c6288fec1bb05b0a0b601d55bf36b73ea',
}
# These are prospective non-network verification rows, not execution authorization.
TEST_WORK = {
    'supervisor-normal-001': "import os; print('WORK_STARTED',os.getpid(),flush=True)",
    'supervisor-nonzero-001': "import os; print('WORK_STARTED',os.getpid(),flush=True); raise SystemExit(3)",
    'supervisor-timeout-001': "import os,signal,time; print('WORK_STARTED',os.getpid(),flush=True); signal.signal(signal.SIGTERM,signal.SIG_IGN); time.sleep(30)",
    'supervisor-tree-001': "import os,subprocess,sys; print('WORK_STARTED',os.getpid(),flush=True); p=subprocess.Popen([sys.executable,'-I','-B','-c','import time; time.sleep(.4)']); print('OWNED_CHILD',p.pid,flush=True); p.wait(); raise SystemExit(4)",
    'supervisor-parent-term-001': "import os,time; print('WORK_STARTED',os.getpid(),flush=True); time.sleep(30)",
    'supervisor-handshake-reject-001': "print('WORK_STARTED',flush=True)",
    'supervisor-publish-stderr-001': "print('WORK_STARTED',flush=True)",
    'supervisor-pre-handshake-001': "print('WORK_STARTED',flush=True)",
}


def demand(condition, message):
    if not condition:
        raise ValueError(message)


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False).encode()


def hash_bytes(data):
    return hashlib.sha256(data).hexdigest()


def hash_file(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1024 ** 2), b''):
            digest.update(block)
    return digest.hexdigest()


def directory_chain(path):
    path = Path(path).absolute()
    demand('..' not in path.parts, 'parent traversal rejected')
    for part in reversed([path, *path.parents]):
        demand(stat.S_ISDIR(part.lstat().st_mode), 'real directory ancestor required: ' + str(part))
    return path


def read_json(path, expected_sha=None):
    path = Path(path).absolute()
    directory_chain(path.parent)
    before = path.lstat()
    demand(stat.S_ISREG(before.st_mode) and before.st_size <= MAX_JSON, 'bounded regular JSON required')
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        opened = os.fstat(descriptor)
        demand((before.st_dev, before.st_ino, before.st_size) == (opened.st_dev, opened.st_ino, opened.st_size), 'JSON identity drift')
        with os.fdopen(descriptor, 'rb', closefd=False) as stream:
            data = stream.read(MAX_JSON + 1)
        demand(len(data) <= MAX_JSON, 'JSON exceeds read bound')
        after = os.fstat(descriptor)
        demand((opened.st_dev, opened.st_ino, opened.st_size, opened.st_mtime_ns) == (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns), 'JSON changed during read')
        demand(expected_sha is None or hash_bytes(data) == expected_sha, 'JSON SHA mismatch')
        return json.loads(data)
    finally:
        os.close(descriptor)


def publish(path, value):
    with Path(path).open('x') as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())


def namespace_identity(pid='self'):
    status = Path('/proc', str(pid), 'ns/pid').stat()
    return {'device': status.st_dev, 'inode': status.st_ino}


def identity(pid):
    try:
        text = Path('/proc', str(pid), 'stat').read_text()
    except OSError as error:
        if error.errno in (errno.ENOENT, errno.ESRCH):
            return None
        raise
    fields = text[text.rfind(')') + 2:].split()
    demand(len(fields) >= 20, 'process stat incomplete')
    return {'pid': int(pid), 'ppid': int(fields[1]), 'pgid': int(fields[2]), 'sid': int(fields[3]),
            'starttime': int(fields[19]), 'state': fields[0]}


def same_object(before, after):
    return after is not None and all(before[key] == after[key] for key in ('pid', 'starttime', 'pgid', 'sid'))


def scan_group(group, namespace, deadline_ns=None):
    """Observe in the same supervisor, never substitute a later exec's numeric PID."""
    began = time.monotonic_ns()
    result = {'started_ns': began, 'namespace': namespace_identity(), 'pgid': group['pid'],
              'range': '/proc numeric directories in this supervisor namespace', 'entries_seen': 0,
              'alive': [], 'zombies': [], 'vanished': [], 'unknown': []}
    if result['namespace'] != namespace:
        result['unknown'].append({'reason': 'supervisor PID namespace changed'})
    try:
        leader = identity(group['pid'])
        if leader is not None and not same_object(group, leader):
            result['unknown'].append({'reason': 'group leader PID reused or changed', 'actual': leader})
        with os.scandir('/proc') as entries:
            for entry in entries:
                if not entry.name.isdecimal():
                    continue
                result['entries_seen'] += 1
                demand(result['entries_seen'] <= MAX_PROC_ENTRIES, 'process scan bound exceeded')
                demand(deadline_ns is None or time.monotonic_ns() < deadline_ns, 'process scan deadline exceeded')
                try:
                    current = identity(int(entry.name))
                    if current is None:
                        result['vanished'].append(int(entry.name))
                        continue
                    if current['pgid'] != group['pid']:
                        continue
                    if current['sid'] != group['sid'] or current['starttime'] < group['starttime'] or namespace_identity(current['pid']) != namespace:
                        result['unknown'].append({'reason': 'group member identity/namespace not attributable', 'identity': current})
                        continue
                    second = identity(current['pid'])
                    if second is None:
                        result['vanished'].append(current['pid'])
                    elif not same_object(current, second):
                        result['unknown'].append({'reason': 'member changed during observation', 'before': current, 'after': second})
                    else:
                        result['zombies' if second['state'] == 'Z' else 'alive'].append(second)
                except OSError as error:
                    if error.errno in (errno.ENOENT, errno.ESRCH):
                        result['vanished'].append(int(entry.name))
                    else:
                        result['unknown'].append({'pid': int(entry.name), 'type': type(error).__name__, 'errno': error.errno, 'message': str(error)})
                except (ValueError, IndexError) as error:
                    result['unknown'].append({'pid': int(entry.name), 'type': type(error).__name__, 'message': str(error)})
    except BaseException as error:
        result['unknown'].append({'type': type(error).__name__, 'message': str(error)})
    result['ended_ns'] = time.monotonic_ns()
    result['complete'] = not result['alive'] and not result['zombies'] and not result['unknown']
    return result


def check_row(table, role, step, maximum, control_root, expected_row_sha):
    demand(type(maximum) is int and maximum > 0, 'integer maximum required')
    root = directory_chain(STAGE / role)
    fixture = table.get('schema') in ('r029-fixture-rows-v1', 'r030-fixture-rows-v1')
    fixture_leaf = 'r030-supervisor-check-001' if table.get('schema') == 'r030-fixture-rows-v1' else 'supervisor-check-001'
    expected_control = root / 'control' / fixture_leaf if fixture else root / 'control'
    demand(Path(control_root).absolute() == expected_control, 'exact control root required')
    directory_chain(expected_control)
    demand(table.get('schema') in ('r028-commands-v1', 'r029-fixture-rows-v1', 'r030-fixture-rows-v1'), 'command table schema rejected')
    matches = [row for row in table['rows'] if row.get('role') == role and row.get('step') == step]
    demand(len(matches) == 1, 'one exact role/step row required')
    row = matches[0]
    demand(hash_bytes(canonical(row)) == expected_row_sha, 'frozen complete row SHA mismatch')
    demand(row['maximum_seconds'] == maximum and type(row['maximum_seconds']) is int, 'row maximum mismatch')
    demand(row['cwd'] == str(REPOSITORY) and row.get('once') is True and row.get('failure_stops_all') is True, 'fixed row execution contract mismatch')
    environment = {'TMPDIR': str(root / 'tmp'), 'TMP': str(root / 'tmp'), 'TEMP': str(root / 'tmp'),
                   'XDG_CACHE_HOME': str(root / 'cache'), 'PYTHONDONTWRITEBYTECODE': '1',
                   'NO_PROXY': '127.0.0.1,localhost', 'LD_LIBRARY_PATH': str(REPOSITORY / '.cache/v0.5-s4/tools/root/usr/lib/x86_64-linux-gnu'), 'PATH': '/usr/bin:/bin'}
    demand(row['environment'] == environment, 'complete fixed environment mismatch')
    if fixture:
        demand(step in TEST_WORK and maximum == 12, 'fixture step/maximum rejected')
        inner = ['/usr/bin/python3', '-I', '-B', '-c', TEST_WORK[step]]
        demand(row['argv'] == inner, 'fixture argv must equal fixed non-network program')
    else:
        choices = {'contract-001': 30, 'build-C': 240, 'build-D': 240, 'smoke-C': 45, 'smoke-D': 45}
        demand(step in choices and maximum == choices[step], 'fixed R028 step/maximum rejected')
        prefix = ['/usr/bin/time', '-f', '%e', '-o', str(root / 'control' / (step + '.wall')),
                  '/usr/bin/timeout', '--signal=TERM', '--kill-after=7s', str(maximum - 7) + 's']
        if step == 'contract-001':
            inner = ['/usr/bin/python3', '-B', str(REPOSITORY / 'benchmark/revalidation/test_revalidate.py'), '--output', str(root / 'contract-001')]
        else:
            action, label = step.split('-')
            inner = ['/usr/bin/python3', '-B', str(REPOSITORY / 'benchmark/revalidation/revalidate.py'), '--role', role, action, '--label', label]
            if action == 'smoke':
                inner += ['--output', str(root / ('smoke-' + label + '-001'))]
        demand(row['argv'] == prefix + inner, 'complete fixed outer/inner argv mismatch')
    return row, inner, fixture


def measure_tree(root):
    allocated = logical_logs = 0
    seen = set()
    vanished = []
    def visit(descriptor, relative):
        nonlocal allocated, logical_logs
        allocated += os.fstat(descriptor).st_blocks * 512
        with os.scandir(descriptor) as entries:
            for entry in entries:
                try:
                    status = os.stat(entry.name, dir_fd=descriptor, follow_symlinks=False)
                except FileNotFoundError:
                    vanished.append(str(relative / entry.name))
                    continue  # Transient build unlink; final observer records the stable snapshot separately.
                if stat.S_ISDIR(status.st_mode):
                    try:
                        child = os.open(entry.name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=descriptor)
                    except FileNotFoundError:
                        vanished.append(str(relative / entry.name))
                        continue
                    try:
                        opened = os.fstat(child)
                        demand((opened.st_dev, opened.st_ino) == (status.st_dev, status.st_ino), 'measurement directory identity changed')
                        visit(child, relative / entry.name)
                    finally:
                        os.close(child)
                elif stat.S_ISREG(status.st_mode):
                    key = (status.st_dev, status.st_ino)
                    if key not in seen:
                        seen.add(key)
                        allocated += status.st_blocks * 512
                        if entry.name.endswith(('.stdout', '.stderr', '.log')):
                            logical_logs += status.st_size
                elif stat.S_ISLNK(status.st_mode):
                    allocated += status.st_blocks * 512
                else:
                    raise ValueError('special object in role tree')
    descriptor = os.open(directory_chain(root), os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        visit(descriptor, Path('.'))
    finally:
        os.close(descriptor)
    demand(allocated <= 2 * 1024 ** 3, 'role allocated limit exceeded')
    demand(logical_logs <= 64 * 1024 ** 2, 'role logical log limit exceeded')
    return {'allocated_bytes': allocated, 'log_logical_bytes': logical_logs, 'polling_not_hard_quota': True,
            'vanished_during_scan': vanished}


def pipe_json(descriptor, deadline):
    data = bytearray()
    while time.monotonic_ns() < deadline:
        readable, _, _ = select.select([descriptor], [], [], min(.1, max(0, (deadline - time.monotonic_ns()) / 1e9)))
        if not readable:
            continue
        block = os.read(descriptor, 16384 - len(data))
        demand(bool(block), 'handshake EOF before complete message')
        data.extend(block)
        demand(len(data) < 16384, 'handshake size limit')
        if b'\n' in data:
            demand(data.endswith(b'\n') and data.count(b'\n') == 1, 'handshake framing rejected')
            return json.loads(data)
    raise TimeoutError('handshake deadline')


def write_pipe(descriptor, value):
    data = canonical(value) + b'\n'
    demand(len(data) < 4096, 'atomic pipe message bound')
    demand(os.write(descriptor, data) == len(data), 'pipe short write')


def worker(args):
    invocation = read_json(args.spec, args.spec_sha256)
    table = read_json(invocation['table_path'], invocation['table_sha256'])
    row, expected_argv, _ = check_row(table, invocation['role'], invocation['step'],
                                    invocation['maximum_seconds'], invocation['control_root'], invocation['row_sha256'])
    expected_output = Path(invocation['control_root']) / ('r029-' + invocation['step'] + '-001')
    demand(Path(args.spec).absolute() == expected_output / 'invocation.json', 'worker spec path rejected')
    demand(invocation['work_argv'] == expected_argv and invocation['environment'] == row['environment'], 'worker fixed row binding rejected')
    me = identity(os.getpid())
    parent = identity(os.getppid())
    namespace = namespace_identity()
    demand(me is not None and parent is not None and me['pgid'] == parent['pid'] == parent['pgid'], 'worker did not enter timeout group')
    message = {'nonce': invocation['nonce'], 'worker': me, 'timeout': parent, 'namespace': namespace}
    write_pipe(args.notify_fd, message)
    approval = pipe_json(args.ack_fd, invocation['handshake_deadline_ns'])
    demand(approval.get('approved') is True and approval.get('nonce') == invocation['nonce'], 'handshake approval rejected')
    demand(approval['timeout'] == parent and namespace == invocation['namespace'], 'approval identity mismatch')
    demand(same_object(parent, identity(parent['pid'])) and same_object(me, identity(me['pid'])), 'worker identity changed before exec')
    os.close(args.notify_fd)
    os.close(args.ack_fd)
    os.execvpe(invocation['work_argv'][0], invocation['work_argv'], invocation['environment'])


def preflight(root):
    import shutil
    memory = {line.split(':')[0]: int(line.split()[1]) for line in Path('/proc/meminfo').read_text().splitlines()}
    demand(memory['MemAvailable'] >= 1048576, 'MemAvailable below1GiB')
    demand(shutil.disk_usage(root).free >= 4 * 1024 ** 3, 'disk free below4GiB')
    limit = resource.getrlimit(resource.RLIMIT_NOFILE)[0]
    demand(limit == resource.RLIM_INFINITY or limit >= 256, 'nofile below256')
    return {'MemAvailable_kib': memory['MemAvailable'], 'nofile': limit}


def parse_wall(text):
    lines = text.strip().splitlines()
    demand(bool(lines) and re.fullmatch(r'[0-9]+(?:\.[0-9]+)?', lines[-1]) is not None, 'time wall missing/invalid')
    for line in lines[:-1]:
        demand(re.fullmatch(r'Command (?:exited with non-zero status [0-9]+|terminated by signal [0-9]+)', line) is not None, 'unrecognized time diagnostic')
    value = float(lines[-1])
    demand(math.isfinite(value) and value >= 0, 'time wall nonfinite/negative')
    return value


def verify_inputs(path, expected_sha):
    manifest = read_json(path, expected_sha)
    demand(manifest.get('schema') == 'r029-static-inputs-v1', 'input manifest schema rejected')
    required = {str(Path(__file__).absolute()), str(REPOSITORY / 'benchmark/revalidation/test_supervise.py'),
                *[str(REPOSITORY / name) for name in OLD_INPUTS]}
    supplied = set()
    for entry in manifest['files']:
        demand(entry['path'] not in supplied, 'duplicate sealed input')
        supplied.add(entry['path'])
        demand(hash_file(entry['path']) == entry['sha256'], 'sealed input drift: ' + entry['path'])
    demand(required <= supplied, 'actual Python/legacy input closure incomplete')
    return manifest


def supervise(args, started_ns):
    output = None
    process = None
    time_identity = None
    group = None
    namespace = namespace_identity()
    parent_identity = identity(os.getpid())
    failure = None
    cleanup_errors = []
    scans = []
    actions = []
    streams = []
    descriptors = []
    known = {}
    pre_group_owned = {}
    pre_group_observations = []
    registration_unknown = False
    spawn_attempted = False
    wall_seconds = None
    returncode = None
    result = None
    evidence_complete = True
    deadline = started_ns + args.maximum_seconds * 1000000000
    # Preserve seven-second GNU timeout escalation plus final same-parent observation reserve.
    final_reserve = 8 if args.maximum_seconds >= 23 else 3
    term_at = deadline - (7 + final_reserve) * 1000000000
    handshake_at = min(term_at, started_ns + 5 * 1000000000)
    previous_term = signal.getsignal(signal.SIGTERM)
    def interrupted(signum, frame):
        raise TimeoutError('supervisor received TERM')
    signal.signal(signal.SIGTERM, interrupted)
    def retain(error):
        nonlocal failure
        # Error formatting/reporting must never become another escaping exception.
        try:
            try:
                message = str(error)
            except BaseException:
                message = 'error message unavailable'
            entry = {'type': type(error).__name__, 'errno': getattr(error, 'errno', None), 'message': message}
            if failure is None:
                failure = entry
            else:
                cleanup_errors.append(entry)
        except BaseException:
            if failure is None:
                failure = {'type': 'ReportingFailure', 'errno': None, 'message': 'error preservation failed'}
    def diagnostic(message):
        try:
            print(message, file=sys.stderr)
        except BaseException as error:
            retain(error)
    def save_evidence(name, value):
        nonlocal evidence_complete
        if output is not None:
            try:
                publish(output / name, value)
                return True
            except BaseException as error:
                evidence_complete = False
                retain(error)
                diagnostic('supervisor evidence write failed: ' + name)
        return False
    def register_pre_group():
        nonlocal time_identity, registration_unknown
        if process is None:
            return
        current = identity(process.pid)
        if time_identity is None:
            demand(current is not None and current['ppid'] == parent_identity['pid'], 'spawn-to-time-registration ownership unknown')
            demand(namespace_identity(current['pid']) == namespace, 'time registration namespace mismatch')
            time_identity = current
        demand(same_object(time_identity, current), 'time registration identity changed')
        demand(namespace_identity(current['pid']) == namespace, 'time registration namespace mismatch')
        stop = min(handshake_at, time.monotonic_ns() + 500000000)
        while time.monotonic_ns() < stop:
            children = Path('/proc', str(process.pid), 'task', str(process.pid), 'children').read_text().split()
            demand(len(children) <= 16, 'pre-handshake child bound exceeded')
            for child in children:
                entry = identity(int(child))
                if entry is None or entry['ppid'] != process.pid:
                    continue
                demand(namespace_identity(entry['pid']) == namespace, 'timeout registration namespace mismatch')
                # Wait until timeout has established its own group; never freeze its inherited group.
                if entry['pgid'] != entry['pid']:
                    continue
                demand(same_object(entry, identity(entry['pid'])), 'timeout registration identity unstable')
                pre_group_owned[(entry['pid'], entry['starttime'])] = entry
                pre_group_observations.append({'stage': 'before_handshake', 'identity': entry, 'source_parent': time_identity,
                                               'namespace': namespace, 'at_ns': time.monotonic_ns()})
                descendants = Path('/proc', str(entry['pid']), 'task', str(entry['pid']), 'children').read_text().split()
                demand(len(descendants) <= 16, 'pre-handshake worker bound exceeded')
                for descendant in descendants:
                    worker_entry = identity(int(descendant))
                    if worker_entry is not None and worker_entry['ppid'] == entry['pid']:
                        demand(worker_entry['pgid'] == entry['pid'] and namespace_identity(worker_entry['pid']) == namespace,
                               'pre-handshake worker identity mismatch')
                        pre_group_owned[(worker_entry['pid'], worker_entry['starttime'])] = worker_entry
                        pre_group_observations.append({'stage': 'before_handshake_worker', 'identity': worker_entry,
                                                       'source_parent': entry, 'namespace': namespace, 'at_ns': time.monotonic_ns()})
            if pre_group_owned:
                return
            time.sleep(.005)
        registration_unknown = True
        raise ValueError('timeout pre-handshake registration unavailable')
    def reclaim_pre_group(signum):
        for entry in reversed(list(pre_group_owned.values())):
            try:
                demand(time.monotonic_ns() < deadline - 1000000000, 'pre-group cleanup deadline exhausted')
                current = identity(entry['pid'])
                if current is not None:
                    demand(same_object(entry, current), 'pre-group registered identity changed')
                    demand(namespace_identity(current['pid']) == namespace, 'pre-group signal namespace mismatch')
                    demand(current['pid'] != parent_identity['pid'] and current['pgid'] != os.getpgrp(), 'pre-group refuses parent group')
                    if current['state'] != 'Z':
                        os.kill(current['pid'], signum)
                        actions.append({'stage': 'pre_group_cleanup', 'signal': signum, 'identity': current, 'at_ns': time.monotonic_ns()})
            except BaseException as error:
                retain(error)
    def observe():
        if group is None:
            return None
        observation = scan_group(group, namespace, deadline - 1000000000)
        scans.append(observation)
        leader = identity(group['pid'])
        leader_valid = same_object(group, leader)
        for entry in observation['alive'] + observation['zombies']:
            key = (entry['pid'], entry['starttime'])
            if leader_valid or key in known:
                known[key] = entry
            else:
                observation['unknown'].append({'reason': 'new member after leader exit not attributable', 'identity': entry})
        observation['complete'] = not observation['alive'] and not observation['zombies'] and not observation['unknown']
        return observation
    def signal_owned(signum):
        for entry in list(known.values()):
            try:
                demand(time.monotonic_ns() < deadline - 1000000000, 'owned signal cleanup deadline exhausted')
                current = identity(entry['pid'])
                if same_object(entry, current) and current['state'] != 'Z':
                    demand(current['pgid'] == group['pid'] and current['pgid'] != os.getpgrp(), 'refuse signal to supervisor group')
                    demand(namespace_identity(current['pid']) == namespace, 'signal namespace mismatch')
                    os.kill(current['pid'], signum)
                    actions.append({'signal': signum, 'identity': current, 'at_ns': time.monotonic_ns()})
            except OSError as error:
                if error.errno not in (errno.ENOENT, errno.ESRCH):
                    retain(error)
            except BaseException as error:
                retain(error)
    try:
        table = read_json(args.table, args.table_sha256)
        row, inner, fixture = check_row(table, args.role, args.step, args.maximum_seconds, args.output_control_root, args.row_sha256)
        root = directory_chain(STAGE / args.role)
        target = Path(args.output_control_root) / ('r029-' + args.step + '-001')
        target.mkdir(exist_ok=False)
        output = target
        publish(output / 'intent.json', {'schema': 'r029-intent-v1', 'row': row, 'row_sha256': args.row_sha256,
                'table_path': args.table, 'table_sha256': args.table_sha256, 'maximum_seconds': args.maximum_seconds,
                'inputs_path': args.inputs, 'inputs_sha256': args.inputs_sha256,
                'started_ns': started_ns, 'clock': 'CLOCK_MONOTONIC', 'namespace': namespace,
                'supervisor': parent_identity, 'once_consumed': True, 'dynamic_authorization_external': True})
        resources = preflight(root)
        verify_inputs(args.inputs, args.inputs_sha256)
        for path, expected in OLD_INPUTS.items():
            demand(hash_file(REPOSITORY / path) == expected, 'frozen old source drift: ' + path)
        initial_bytes = measure_tree(root)
        demand(time.monotonic_ns() < handshake_at, 'preparation consumed handshake allowance')
        notify_read, notify_write = os.pipe()
        ack_read, ack_write = os.pipe()
        descriptors.extend([notify_read, notify_write, ack_read, ack_write])
        nonce = os.urandom(16).hex()
        invocation = {'nonce': nonce, 'work_argv': inner, 'environment': row['environment'],
                      'namespace': namespace, 'handshake_deadline_ns': handshake_at,
                      'table_path': args.table, 'table_sha256': args.table_sha256, 'row_sha256': args.row_sha256,
                      'role': args.role, 'step': args.step, 'maximum_seconds': args.maximum_seconds,
                      'control_root': args.output_control_root}
        publish(output / 'invocation.json', invocation)
        command = ['/usr/bin/time', '-f', '%e', '-o', str(output / 'time.wall'),
                   '/usr/bin/timeout', '--signal=TERM', '--kill-after=7s',
                   str(max(.001, (term_at - time.monotonic_ns()) / 1e9)) + 's',
                   '/usr/bin/python3', '-B', str(Path(__file__).absolute()), '__worker',
                   '--spec', str(output / 'invocation.json'), '--spec-sha256', hash_file(output / 'invocation.json'),
                   '--notify-fd', str(notify_write), '--ack-fd', str(ack_read)]
        demand(save_evidence('launch.json', {'argv': command, 'environment': row['environment'], 'resources': resources,
                                    'initial_bytes': initial_bytes, 'term_at_ns': term_at, 'deadline_ns': deadline}), 'launch evidence unavailable')
        for name in ('work.stdout', 'work.stderr'):
            streams.append((output / name).open('xb'))
        spawn_attempted = True
        process = subprocess.Popen(command, cwd=REPOSITORY, env=row['environment'], stdout=streams[0], stderr=streams[1],
                                   pass_fds=(notify_write, ack_read))
        time_identity = identity(process.pid)
        demand(time_identity is not None and time_identity['ppid'] == parent_identity['pid'], 'time child identity unavailable')
        register_pre_group()
        for descriptor in (notify_write, ack_read):
            os.close(descriptor); descriptors.remove(descriptor)
        hello = pipe_json(notify_read, handshake_at)
        timeout_actual = identity(hello['timeout']['pid'])
        worker_actual = identity(hello['worker']['pid'])
        demand(same_object(hello['timeout'], timeout_actual) and same_object(hello['worker'], worker_actual), 'handshake actual identity mismatch')
        demand(timeout_actual['ppid'] == process.pid and worker_actual['ppid'] == timeout_actual['pid'], 'time/timeout/worker parent chain mismatch')
        demand(timeout_actual['pgid'] == timeout_actual['pid'] == worker_actual['pgid'] and timeout_actual['pgid'] != os.getpgrp(), 'final timeout group rejected')
        demand(hello['nonce'] == nonce and hello['namespace'] == namespace and namespace_identity(timeout_actual['pid']) == namespace and namespace_identity(worker_actual['pid']) == namespace, 'handshake namespace/nonce mismatch')
        group = timeout_actual
        observation = observe()
        demand(not observation['unknown'], 'start group observation unknown')
        demand(save_evidence('handshake.json', {'hello': hello, 'actual_timeout': timeout_actual, 'actual_worker': worker_actual,
                                        'supervisor': parent_identity, 'time': time_identity, 'namespace': namespace,
                                        'group_confirmed_ns': time.monotonic_ns()}), 'handshake evidence unavailable')
        if fixture and args.step == 'supervisor-handshake-reject-001':
            write_pipe(ack_write, {'approved': False, 'nonce': nonce})
            raise ValueError('fixture handshake rejection before work')
        demand(time.monotonic_ns() < handshake_at, 'handshake completed too late')
        write_pipe(ack_write, {'approved': True, 'nonce': nonce, 'timeout': hello['timeout']})
        os.close(ack_write); descriptors.remove(ack_write)
        demand(save_evidence('work-release.json', {'released_ns': time.monotonic_ns(), 'nonce': nonce}), 'work release evidence unavailable')
        next_poll = time.monotonic_ns()
        while process.poll() is None:
            demand(time.monotonic_ns() < deadline - final_reserve * 1000000000, 'time/timeout exceeded reserved wait boundary')
            if time.monotonic_ns() >= next_poll:
                measure_tree(root)
                observation = observe()
                demand(not observation['unknown'], 'runtime group observation unknown')
                next_poll = time.monotonic_ns() + 250000000
            time.sleep(.02)
        returncode = process.wait(timeout=max(.001, (deadline - time.monotonic_ns()) / 1e9))
        save_evidence('wait.json', {'returncode': returncode, 'wait_completed_ns': time.monotonic_ns(), 'time_identity': time_identity, 'namespace': namespace_identity()})
        demand(returncode == 0, 'time/timeout/work nonzero exit: ' + str(returncode))
    except BaseException as error:
        retain(error)
    finally:
        try:
            signal.signal(signal.SIGTERM, signal.SIG_IGN)
        except BaseException as error:
            retain(error)
        # Closing approval pipes prevents work after failed or absent handshake.
        for descriptor in list(descriptors):
            try:
                os.close(descriptor)
            except BaseException as error:
                retain(error)
        descriptors.clear()
        if spawn_attempted and process is None:
            registration_unknown = True
            retain(ValueError('spawn attempted without returned child handle; ownership unknown'))
        if process is not None and not pre_group_owned:
            try:
                register_pre_group()
            except BaseException as error:
                registration_unknown = True
                retain(error)
        if group is None:
            reclaim_pre_group(signal.SIGTERM)
            if process is not None:
                try:
                    stop_at = min(deadline - 1000000000, time.monotonic_ns() + 500000000)
                    while process.poll() is None and time.monotonic_ns() < stop_at:
                        time.sleep(.005)
                except BaseException as error:
                    retain(error)
        final_scan = None
        if group is not None:
            try:
                final_scan = observe()
            except BaseException as error:
                retain(error)
            if final_scan is not None and final_scan['alive'] and time.monotonic_ns() < deadline - 1000000000:
                signal_owned(signal.SIGTERM)
                stop_at = min(deadline - 1000000000, time.monotonic_ns() + 1000000000)
                while time.monotonic_ns() < stop_at:
                    try:
                        final_scan = observe()
                        if final_scan['complete']:
                            break
                    except BaseException as error:
                        retain(error); break
                    time.sleep(.02)
                if final_scan is not None and final_scan['alive']:
                    signal_owned(signal.SIGKILL)
        if process is not None:
            try:
                if process.poll() is None and time.monotonic_ns() < deadline - 1000000000:
                    # Only the still-owned direct time child; group signals use registered members above.
                    demand(time_identity is not None and same_object(time_identity, identity(process.pid)), 'time identity unknown; no signal')
                    process.terminate()
                    try:
                        process.wait(timeout=min(1, max(.001, (deadline - 1000000000 - time.monotonic_ns()) / 1e9)))
                    except subprocess.TimeoutExpired:
                        demand(same_object(time_identity, identity(process.pid)), 'time kill identity changed')
                        demand(time.monotonic_ns() < deadline - 1000000000, 'time kill cleanup deadline exhausted')
                        process.kill()
                        process.wait(timeout=max(.001, (deadline - 1000000000 - time.monotonic_ns()) / 1e9))
                elif process.poll() is not None:
                    process.wait(timeout=.01)
                returncode = process.returncode
            except BaseException as error:
                retain(error)
        if group is not None:
            try:
                while time.monotonic_ns() < deadline - 1000000000:
                    final_scan = observe()
                    if final_scan['complete'] or final_scan['unknown']:
                        break
                    time.sleep(.02)
                final_scan = observe()  # Actual same-parent observation after direct-child wait.
            except BaseException as error:
                retain(error)
                final_scan = None
        pre_group_remaining = []
        if group is None:
            reclaim_pre_group(signal.SIGKILL)
        for entry in list(pre_group_owned.values()):
            try:
                stop = deadline - 1000000000
                while time.monotonic_ns() < stop:
                    current = identity(entry['pid'])
                    if current is None:
                        break
                    demand(same_object(entry, current) and namespace_identity(current['pid']) == namespace,
                           'post-wait pre-group identity unknown')
                    time.sleep(.005)
                current = identity(entry['pid'])
                pre_group_observations.append({'stage': 'post_wait', 'registered': entry, 'current': current,
                                               'namespace': namespace_identity(), 'at_ns': time.monotonic_ns()})
                if current is not None:
                    pre_group_remaining.append(current)
            except BaseException as error:
                registration_unknown = True
                retain(error)
                pre_group_remaining.append({'registered': entry, 'unknown': True})
        for stream in streams:
            try:
                stream.close()
            except BaseException as error:
                retain(error)
        if output is not None:
            save_evidence('group-observations.json', {'observations': scans, 'actions': actions,
                          'namespace': namespace, 'supervisor': parent_identity, 'time': time_identity,
                          'pre_group_observations': pre_group_observations, 'registration_unknown': registration_unknown,
                          'pre_group_remaining': pre_group_remaining,
                          'ability_boundary': 'fixed adapters forbid setsid/foreground; group scan does not prove arbitrary malicious escaped descendants absent'})
            try:
                data = (output / 'time.wall').read_text()
                wall_seconds = parse_wall(data)
            except BaseException as error:
                retain(error)
            try:
                final_bytes = measure_tree(STAGE / args.role)
                demand(not final_bytes['vanished_during_scan'], 'final role measurement not stable')
                save_evidence('final-space.json', final_bytes)
            except BaseException as error:
                retain(error)
            for path, expected in OLD_INPUTS.items():
                try:
                    demand(hash_file(REPOSITORY / path) == expected, 'old input changed during supervisor')
                except BaseException as error:
                    retain(error)
            try:
                verify_inputs(args.inputs, args.inputs_sha256)
            except BaseException as error:
                retain(error)
        ended_ns = time.monotonic_ns()
        direct_reaped = False
        try:
            direct_reaped = process is not None and process.poll() is not None
        except BaseException as error:
            retain(error)
        cleanup_complete = group is not None and final_scan is not None and final_scan['complete'] and direct_reaped and not cleanup_errors and not registration_unknown and not pre_group_remaining
        complete_boundary = ended_ns <= deadline and output is not None and wall_seconds is not None and cleanup_complete and evidence_complete
        status = 'valid' if failure is None and complete_boundary and returncode == 0 else ('invalid' if complete_boundary else 'unknown')
        result = {'schema': 'r029-exit-v1', 'status': status, 'first_error': failure, 'cleanup_errors': cleanup_errors,
                  'supervisor': parent_identity, 'namespace': namespace, 'group': group, 'time': time_identity,
                  'returncode': returncode, 'cleanup_complete': cleanup_complete, 'final_group_observation': final_scan,
                  'pre_group_observations': pre_group_observations, 'pre_group_remaining': pre_group_remaining,
                  'registration_unknown': registration_unknown,
                  'started_ns': started_ns, 'ended_ns': ended_ns, 'clock': 'CLOCK_MONOTONIC',
                  'complete_elapsed_seconds': (ended_ns - started_ns) / 1e9,
                  'time_wall_seconds': wall_seconds, 'time_wall_scope': 'time child launch to timeout subtree return; excludes supervisor preparation/final observation',
                  'charged_seconds': math.ceil((ended_ns - started_ns) / 1e9) if complete_boundary else args.maximum_seconds,
                  'charge_kind': 'complete_elapsed_ceiling' if complete_boundary else 'conservative_maximum_unknown',
                  'maximum_seconds': args.maximum_seconds, 'all_following_steps_must_stop': status != 'valid'}
        # Exit evidence precedes authoritative settlement; its publication time is included below.
        evidence_saved = save_evidence('exit.json', dict(result, finalized=False))
        ended_ns = time.monotonic_ns()
        complete_boundary = complete_boundary and evidence_saved and ended_ns <= deadline and not cleanup_errors
        result.update({'finalized': True, 'ended_ns': ended_ns,
                       'complete_elapsed_seconds': (ended_ns - started_ns) / 1e9,
                       'first_error': failure, 'cleanup_errors': cleanup_errors,
                       'status': ('valid' if failure is None and returncode == 0 else 'invalid') if complete_boundary else 'unknown',
                       'charged_seconds': math.ceil((ended_ns - started_ns) / 1e9) if complete_boundary else args.maximum_seconds,
                       'charge_kind': 'complete_elapsed_ceiling' if complete_boundary else 'conservative_maximum_unknown',
                       'all_following_steps_must_stop': not complete_boundary or failure is not None or returncode != 0,
                       'endpoint_scope': 'after exclusive exit evidence, all group observations, cleanup and space checks; before final settlement serialization'})
        settlement_saved = save_evidence('settlement.json', result)
        if not settlement_saved or time.monotonic_ns() > deadline:
            result['status'] = 'unknown'
            result['charged_seconds'] = args.maximum_seconds
            result['all_following_steps_must_stop'] = True
            diagnostic('supervisor settlement publication failed/exceeded total deadline; conservative maximum')
        try:
            signal.signal(signal.SIGTERM, previous_term)
        except BaseException as error:
            retain(error)
            result['status'] = 'unknown'
    if result is None or result['status'] != 'valid':
        raise RuntimeError('R029 supervisor did not validate; stop all following steps')
    return result


def main(argv=None):
    started_ns = time.monotonic_ns()  # Includes parsing, resource/identity checks and intent publication.
    argv = sys.argv[1:] if argv is None else argv
    if argv and argv[0] == '__worker':
        parser = argparse.ArgumentParser()
        parser.add_argument('--spec', required=True)
        parser.add_argument('--spec-sha256', required=True)
        parser.add_argument('--notify-fd', type=int, required=True)
        parser.add_argument('--ack-fd', type=int, required=True)
        worker(parser.parse_args(argv[1:]))
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--role', choices=('builder', 'reviewer'), required=True)
    parser.add_argument('--step', required=True)
    parser.add_argument('--table', required=True)
    parser.add_argument('--table-sha256', required=True)
    parser.add_argument('--row-sha256', required=True)
    parser.add_argument('--output-control-root', required=True)
    parser.add_argument('--maximum-seconds', type=int, required=True)
    parser.add_argument('--inputs', required=True)
    parser.add_argument('--inputs-sha256', required=True)
    supervise(parser.parse_args(argv), started_ns)


if __name__ == '__main__':
    main()
