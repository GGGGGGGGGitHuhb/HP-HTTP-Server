#!/usr/bin/env python3
"""S3: four bounded coarse thread/syscall observations of fixed S1."""
import argparse
import json
import math
import os
import pathlib
import resource
import shutil
import signal
import subprocess
import sys
import time
import types
import socket

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'benchmark/matrix'))
import Build as MatrixBuild
import Common as M
import Run as MatrixRun

SEQUENCE = [('M2', False), ('M2', True), ('M6', False)]
LEGACY_SEQUENCE = SEQUENCE + [('M6', True)]
CAPS = {'fastchecks': 30, 'probe': 20, 'formal': 120}
STRACE = pathlib.Path('/usr/bin/strace')


def role_root(path):
    root = pathlib.Path(path).resolve()
    ordinary = root.parent == REPO / '.cache/v0.6-s3' and root.name in ('builder', 'reviewer')
    recovery = root in (REPO / '.cache/v0.6-s3/builder/rework-001', REPO / '.cache/v0.6-s3/builder/rework-002')
    M.demand((ordinary or recovery) and root.is_dir(), 'S3 role root mismatch')
    return root


class Budget:
    def __init__(self, root, create=False):
        self.charge_root = pathlib.Path(root).parent if pathlib.Path(root).name in ('rework-001', 'rework-002') else pathlib.Path(root)
        self.root, self.path = pathlib.Path(root), pathlib.Path(root) / 'budget.json'
        if create:
            M.demand(not self.path.exists(), 'S3 budget already started')
            now = time.monotonic()
            soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
            M.demand(shutil.disk_usage(root).free >= 256 * 1024**2 and soft >= 64, 'S3 disk/nofile unavailable')
            self.data = {'started_monotonic': now, 'deadline_monotonic': now + (60 if self.root.name == 'rework-002' else 180), 'phases': {},
                         'boot_id': pathlib.Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
                         'disk_free_before': shutil.disk_usage(root).free, 'nofile': [soft, hard],
                         'approved_design_sha256': M.sha(REPO / 'docs/leader/designs/V0.6/S3-design.md'),
                         'limits': {'total_s': 60 if self.root.name == 'rework-002' else 180, 'cleanup_s': 10, 'task_bytes': 256 * 1024**2, 'raw_bytes': 64 * 1024**2}}
            if self.charge_root != self.root:
                self.data['recovery'] = {'name': 'S3-' + self.root.name, 'approved_sha256': M.sha(REPO / ('docs/leader/reworks/V0.6/S3-' + self.root.name + '.md')),
                    'old_budget_sha256': M.sha(self.charge_root / 'budget.json'), 'old_settlement_sha256': M.sha(self.charge_root / 'budget-settlement.json')}
            if self.root.name == 'rework-002':
                self.data['recovery']['r001_ledgers'] = {n:M.sha(self.charge_root / 'rework-001' / n) for n in ('budget.json', 'budget-settlement.json')}
            M.save(self.path, self.data)
        self.data = json.loads(self.path.read_text())
        M.demand(self.data['boot_id'] == pathlib.Path('/proc/sys/kernel/random/boot_id').read_text().strip(), 'S3 host changed')
        self.phase, self.phase_deadline, self.last_scan = None, self.data['deadline_monotonic'] - 10, 0

    def begin(self, phase):
        M.demand(phase not in self.data['phases'], 'S3 phase consumed; no retry')
        self.check()
        self.phase = phase
        now = time.monotonic()
        self.phase_deadline = min(self.data['deadline_monotonic'] - 10, now + (50 if self.root.name == 'rework-002' and phase == 'fastchecks' else CAPS[phase]))
        self.data['phases'][phase] = {'start_monotonic': now, 'deadline_monotonic': self.phase_deadline, 'status': 'running'}
        M.save(self.path, self.data)

    def check(self, deadline=None):
        now = time.monotonic()
        M.demand(now < min(self.phase_deadline, deadline if deadline is not None else self.phase_deadline), 'S3 absolute deadline exhausted')
        if now - self.last_scan >= 1:
            self.data['usage'] = M.sizes(self.charge_root)
            self.last_scan = now
        for key in ('charged_bytes', 'raw_bytes'):
            limit = self.data['limits']['task_bytes' if key == 'charged_bytes' else key]
            M.demand(self.data['usage'][key] <= limit, 'S3 detective space limit exceeded')

    def finish(self, status, error=None):
        now = time.monotonic()
        self.data['phases'][self.phase].update(status=status, error=error, end_monotonic=now,
                                             elapsed_s=now - self.data['phases'][self.phase]['start_monotonic'])
        self.data.update(elapsed_s=now - self.data['started_monotonic'], remaining_s=max(0, self.data['deadline_monotonic'] - now), usage=M.sizes(self.charge_root))
        M.save(self.path, self.data)


def identity(root):
    manifest = MatrixBuild.validate_manifest(REPO / '.cache/v0.6-s2' / (root.parent.name if root.name in ('rework-001', 'rework-002') else root.name) / 'artifact/manifest.json')
    M.demand(STRACE.is_file(), 'strace missing; no install')
    version = subprocess.run([str(STRACE), '--version'], capture_output=True, text=True, timeout=3, check=True).stdout.splitlines()[0]
    M.demand('6.8' in version, 'strace version mismatch')
    return manifest, {'argv': [str(STRACE), '--version'], 'version': version, 'sha256': M.sha(STRACE)}


def threads(pid):
    result = []
    for path in sorted((pathlib.Path('/proc') / str(pid) / 'task').iterdir(), key=lambda p: int(p.name)):
        stat = (path / 'stat').read_text()
        fields = stat.rsplit(')', 1)[1].split()
        frequency = os.sysconf('SC_CLK_TCK')
        result.append({'pid': pid, 'tid': int(path.name), 'starttime': int(fields[19]),
                       'comm': stat.split('(', 1)[1].rsplit(')', 1)[0], 'utime_ticks': int(fields[11]), 'stime_ticks': int(fields[12]),
                       'clock_ticks_per_second': frequency, 'read_monotonic': time.monotonic(),
                       'role': 'main reactor' if int(path.name) == pid else 'background role unknown (worker/logger)'} )
    return result


def thread_cpu(before, after):
    M.demand(before and len(before) == len(after), 'missing thread evidence')
    later = {r['tid']: r for r in after}
    M.demand(len(later) == len(after) and {r['tid'] for r in before} == set(later), 'thread set changed')
    values = []
    for a in before:
        b = later[a['tid']]
        for key in ('pid', 'tid', 'starttime', 'clock_ticks_per_second'):
            M.demand(type(a[key]) is int and a[key] > 0 and a[key] == b[key], 'thread identity/frequency changed')
        for row in (a, b):
            for key in ('utime_ticks', 'stime_ticks'):
                M.demand(type(row[key]) is int and row[key] >= 0, 'invalid thread ticks')
            M.demand(type(row['read_monotonic']) in (int, float) and math.isfinite(row['read_monotonic']), 'invalid thread clock')
        elapsed = b['read_monotonic'] - a['read_monotonic']
        ticks = b['utime_ticks'] + b['stime_ticks'] - a['utime_ticks'] - a['stime_ticks']
        M.demand(elapsed > 0 and ticks >= 0, 'negative thread delta/clock')
        values.append({'tid': a['tid'], 'role': a['role'], 'cpu_seconds': ticks / a['clock_ticks_per_second'],
                       'wall_s': elapsed, 'cpu_pct': 100 * ticks / a['clock_ticks_per_second'] / elapsed})
    return values


def trace_summary(text):
    rows, total = [], None
    for line in text.splitlines():
        parts = line.split()
        if len(parts) not in (5, 6) or not parts[0][0:1].isdigit():
            continue
        pct, seconds, usecs, calls = map(float, parts[:4])
        errors = float(parts[4]) if len(parts) == 6 else 0
        name = parts[-1]
        M.demand(all(math.isfinite(v) and v >= 0 for v in (pct, seconds, usecs, calls, errors)) and calls == int(calls) and errors == int(errors), 'invalid strace scalar')
        row = {'syscall': name, 'percent_system_time': pct, 'system_seconds': seconds, 'usecs_per_call': usecs, 'calls': int(calls), 'errors': int(errors)}
        if name == 'total':
            M.demand(total is None, 'duplicate strace total')
            total = row
        else:
            rows.append(row)
    M.demand(rows and total and total['calls'] > 0, 'missing strace summary')
    M.demand(len({r['syscall'] for r in rows}) == len(rows), 'duplicate strace syscall')
    M.demand(sum(r['calls'] for r in rows) == total['calls'] and sum(r['errors'] for r in rows) == total['errors'], 'strace totals mismatch')
    M.demand(abs(sum(r['system_seconds'] for r in rows) - total['system_seconds']) <= (len(rows) + 1) * 1e-6, 'strace time totals mismatch')
    for row in rows:
        row['computed_percent_system_time'] = 100 * row['system_seconds'] / total['system_seconds'] if total['system_seconds'] else None
        tolerance = 0.02 + 100 * (len(rows) + 1) * 0.5e-6 / total['system_seconds'] if total['system_seconds'] else 0.02
        M.demand(row['computed_percent_system_time'] is None or abs(row['computed_percent_system_time'] - row['percent_system_time']) <= tolerance, 'strace rounded percent mismatch')
    return {'clock_kind': 'default strace system time; not wall or user CPU', 'rows': sorted(rows, key=lambda r: r['system_seconds'], reverse=True), 'total': total}


def validate_relation(server, wrapper, relation, executable, command, expected):
    M.demand(server['pid'] != wrapper['pid'] and server['starttime'] > 0, 'wrapper mistaken for server')
    M.demand(relation['ppid'] == relation['tracer_pid'] == wrapper['pid'], 'tracee wrapper relation mismatch')
    M.demand(executable == str(pathlib.Path(expected[0]).resolve()) and command == expected, 'tracee exe/argv mismatch')


def validate_scope(trace, measurement=None):
    M.demand(trace['trace_scope'] == 'server_lifecycle', 'trace scope mismatch')
    keys = ['launch_requested_monotonic', 'tracee_confirmed_monotonic', 'ready_monotonic']
    if measurement is not None:
        keys += ['warmup_completed_monotonic', 'measurement_started_monotonic', 'measurement_ended_monotonic']
    keys += ['post_audit_completed_monotonic', 'server_term_requested_monotonic', 'wrapper_wait_completed_monotonic', 'tracee_disappearance_verified_monotonic']
    values = [trace[key] for key in keys]
    M.demand(all(type(v) in (int, float) and math.isfinite(v) for v in values) and values == sorted(values), 'trace lifecycle clock ordering')
    if measurement is not None:
        M.demand(trace['measurement_started_monotonic'] == measurement['started_monotonic'] and trace['measurement_ended_monotonic'] == measurement['ended_monotonic'], 'trace measurement boundary mismatch')


class TracedServer:
    """Controller waits wrapper; wrapper owns/waits the actual server tracee."""
    def __init__(self, argv, directory, budget):
        self.budget, self.directory, self.port = budget, directory, None
        self.summary_path = directory / 'trace-summary.stderr'
        self.trace = {'trace_scope': 'server_lifecycle', 'launch_requested_monotonic': time.monotonic()}
        self.wrapper = M.OwnedProcess([str(STRACE), '-f', '-c', '-o', str(self.summary_path), *argv], directory / 'wrapper')
        self.trace['wrapper_identity'] = self.wrapper.identity
        self.stdout_path = self.wrapper.stdout_path
        self.process = types.SimpleNamespace(pid=None)
        self.identity = None
        try:
            deadline = min(time.monotonic() + 3, budget.phase_deadline)
            while self.identity is None:
                budget.check(deadline)
                M.demand(self.wrapper.poll() is None, 'wrapper early exit before tracee confirmation')
                children = (pathlib.Path('/proc') / str(self.wrapper.identity['pid']) / 'task' / str(self.wrapper.identity['pid']) / 'children').read_text().split()
                for pid in children:
                    try:
                        candidate = M.process_info(int(pid))
                        base = pathlib.Path('/proc') / pid
                        executable = str((base / 'exe').resolve())
                        command = (base / 'cmdline').read_bytes().rstrip(b'\0').split(b'\0')
                        command = [item.decode() for item in command]
                        if executable != str(pathlib.Path(argv[0]).resolve()) or command != argv:
                            continue
                        relation = self.relation(int(pid))
                        validate_relation(candidate, self.wrapper.identity, relation, executable, command, argv)
                        self.identity = candidate
                        self.process.pid = int(pid)
                        self.trace.update(server_identity=candidate, relation=relation, server_exe=executable, server_argv=command,
                                          tracee_confirmed_monotonic=time.monotonic())
                        M.save(directory / 'server.process.json', {**candidate, 'argv': command, **relation, 'direct_child': False})
                    except FileNotFoundError:
                        continue
                if self.identity is None:
                    time.sleep(0.01)
            M.demand(self.identity['pid'] != self.wrapper.identity['pid'], 'wrapper mistaken for server')
        except BaseException:
            self.wrapper.close(budget.data['deadline_monotonic'])
            raise

    def relation(self, pid):
        fields = dict(line.split(':', 1) for line in (pathlib.Path('/proc') / str(pid) / 'status').read_text().splitlines() if ':' in line)
        return {'ppid': int(fields['PPid']), 'tracer_pid': int(fields['TracerPid'])}

    def matches(self):
        try:
            current = M.process_info(self.identity['pid'])
            return current['starttime'] == self.identity['starttime'] and os.getpgid(current['pid']) == self.wrapper.identity['pid']
        except (FileNotFoundError, ProcessLookupError):
            return False

    def poll(self):
        status = self.wrapper.poll()
        return status if status is not None else (None if self.matches() else 1)

    def close(self, deadline=None):
        deadline = min(deadline or self.budget.data['deadline_monotonic'], time.monotonic() + 10)
        forced, natural = False, self.wrapper.poll() is None
        tids = [row['tid'] for row in threads(self.identity['pid'])] if self.matches() else []
        self.trace['server_term_requested_monotonic'] = time.monotonic()
        if self.matches():
            os.kill(self.identity['pid'], signal.SIGTERM)
        while (self.wrapper.poll() is None or self.matches()) and time.monotonic() < deadline - 3:
            time.sleep(0.01)
        if self.wrapper.poll() is None or self.matches():
            natural = False
            if self.matches():
                forced = True
                os.kill(self.identity['pid'], signal.SIGKILL)
        wrapper_cleanup = self.wrapper.close(deadline)
        disappeared = not self.matches() and not (pathlib.Path('/proc') / str(self.identity['pid'])).exists()
        tids_gone = all(not (pathlib.Path('/proc') / str(self.identity['pid']) / 'task' / str(tid)).exists() for tid in tids)
        port_closed = True
        if self.port:
            with socket.socket() as peer:
                peer.settimeout(0.1)
                port_closed = peer.connect_ex(('127.0.0.1', self.port)) != 0
        self.trace.update(wrapper_wait_completed_monotonic=time.monotonic(), tracee_disappearance_verified_monotonic=time.monotonic())
        result = {'pid': self.identity['pid'], 'starttime': self.identity['starttime'], 'direct_child': False,
                  'tracee_disappeared': disappeared, 'thread_tids': tids, 'threads_disappeared': tids_gone, 'port_closed': port_closed,
                  'forced': forced or wrapper_cleanup['forced'], 'natural_wrapper_exit': natural,
                  'wrapper_cleanup': wrapper_cleanup, 'wrapper_reaped': wrapper_cleanup['reaped'], 'server_returncode': None}
        M.save(self.directory / 'server.cleanup.json', result)
        M.demand(disappeared and tids_gone and port_closed and natural and not result['forced'] and wrapper_cleanup['returncode'] == 0, 'traced identity-tree cleanup invalid')
        self.trace['summary'] = trace_summary(self.summary_path.read_text())
        return result


def server_start(manifest, payload_root, config, directory, traced=False, budget=None):
    argv = [manifest['binary'], '--port', '0', '--root', str(payload_root), '--threads', str(config['workers']),
            '--idle-timeout-ms', '30000', '--keep-alive-timeout-ms', '15000', '--shutdown-timeout-ms', '5000', '--metrics-on-exit']
    return TracedServer(argv, directory, budget) if traced else M.OwnedProcess(argv, directory / 'server')


def load_phase(server, port, payload, config, seconds, prefix, budget, command=None, server_reader=M.process_info, child_factory=M.OwnedProcess):
    """S3-owned wrapper always persists cached wait4, including after-read failure."""
    environment = os.environ.copy()
    environment.update(LD_LIBRARY_PATH=str(M.LIB), HP_MATRIX_MODE=config['mode'], HP_MATRIX_CONNECTIONS=str(config['connections']), NO_PROXY='127.0.0.1,localhost', no_proxy='127.0.0.1,localhost')
    argv = command or [str(M.WRK), '-t', str(config['threads']), '-c', str(config['connections']), '--timeout', '2s', '--latency', '-d', str(seconds) + 's', '-s', str(MatrixRun.LUA), f'http://127.0.0.1:{port}/{payload["name"]}']
    row = {'status': 'running', 'argv': argv, 'started_monotonic': time.monotonic(), 'started_wall_s': time.time(), 'server_after': None}
    child = None
    primary = None
    try:
        row['server_before'] = server_reader(server.process.pid)
        child = child_factory(argv, prefix, environment)
        row['client_identity'] = child.identity
        samples, next_sample = [], row['started_monotonic']
        while child.poll() is None:
            budget.check(min(row['started_monotonic'] + seconds + 5, budget.phase_deadline))
            M.demand(server.poll() is None and server.matches(), 'server failed during S3 load')
            now = time.monotonic()
            if now >= next_sample:
                observed = server_reader(server.process.pid)
                try:
                    client = M.process_info(child.identity['pid'])
                except FileNotFoundError:
                    continue
                samples.append({'elapsed_s': now - row['started_monotonic'], 'server_rss_kib': observed['rss_kib'], 'client_rss_kib': client['rss_kib']})
                next_sample = now + 1
            time.sleep(0.01)
        row['server_after'] = server_reader(server.process.pid)
        row['summary'] = MatrixRun.parse_summary(child.stdout_path.read_text(), child.process.returncode, payload['size'], config['connections'])
        row['rss_samples'] = samples
    except BaseException as error:
        primary = error
        row.update(status='invalid', error=repr(error))
    finally:
        if child:
            row['cleanup_started_monotonic'] = time.monotonic()
            try:
                row['cleanup'] = child.close(budget.data['deadline_monotonic'])
                row['cpu_evidence'] = MatrixRun.cpu_evidence(row['server_before'], row['server_after'], child)
            except BaseException as error:
                row.update(status='invalid', cleanup_error=repr(error))
                if primary is None:
                    primary = error
        row['cleanup_ended_monotonic'] = time.monotonic()
        row.update(ended_monotonic=time.monotonic(), ended_wall_s=time.time())
        row['wall_s'] = row['ended_monotonic'] - row['started_monotonic']
        if primary is None and child and row['server_after'] is not None:
            row['server_cpu_pct'] = 100 * (row['server_after']['cpu_seconds'] - row['server_before']['cpu_seconds']) / row['wall_s']
            row['client_cpu_pct'] = 100 * (child.usage.ru_utime + child.usage.ru_stime) / row['wall_s']
            row['server_rss_sampled_max_kib'] = max([row['server_before']['rss_kib']] + [r['server_rss_kib'] for r in row['rss_samples']])
            row['client_rss_sampled_max_kib'] = max([child.identity['rss_kib']] + [r['client_rss_kib'] for r in row['rss_samples']])
            row['server_vmhwm_kib'] = row['server_after']['hwm_kib']
            try:
                MatrixRun.validate_cpu_evidence(row, server.identity, child.identity)
                row['status'] = 'valid'
            except BaseException as error:
                row.update(status='invalid', error=repr(error))
                primary = error
        M.save(prefix.with_suffix('.result.json'), row)
    if primary is not None:
        raise primary
    return row


def sample(manifest, config, traced, directory, payload_root, payload, budget, synthetic_load=None):
    row = {'status': 'running', 'config': config, 'traced': traced, 'payload': payload, 'started_monotonic': time.monotonic()}
    server = None
    load = synthetic_load or load_phase
    try:
        server = server_start(manifest, payload_root, config, directory, traced, budget)
        port = MatrixRun.ready(server, budget)
        if traced:
            server.port = port
            row['trace'] = server.trace
            row['trace']['ready_monotonic'] = time.monotonic()
        row['pre_audit'] = MatrixRun.audit(port, payload, config['mode'], budget)
        row['warmup'] = load(server, port, payload, config, 1, directory / 'warmup', budget)
        if traced:
            row['trace']['warmup_completed_monotonic'] = time.monotonic()
        row['threads_before'] = threads(server.identity['pid'])
        row['measurement'] = load(server, port, payload, config, 5, directory / 'measurement', budget)
        if traced:
            row['trace'].update(measurement_started_monotonic=row['measurement']['started_monotonic'], measurement_ended_monotonic=row['measurement']['ended_monotonic'])
        row['threads_after'] = threads(server.identity['pid'])
        row['thread_cpu'] = thread_cpu(row['threads_before'], row['threads_after'])
        row['post_audit'] = MatrixRun.audit(port, payload, config['mode'], budget)
        if traced:
            row['trace']['post_audit_completed_monotonic'] = time.monotonic()
        row['status'] = 'valid'
    except BaseException as error:
        row.update(status='invalid', error=repr(error))
        raise
    finally:
        try:
            if server:
                row['server_cleanup'] = server.close(budget.data['deadline_monotonic'])
                M.demand(not row['server_cleanup']['forced'] and (traced or row['server_cleanup']['returncode'] == 0), 'server cleanup invalid')
                row['metrics'] = MatrixRun.parse_metrics(server.stdout_path.read_text())
                if traced and 'measurement' in row:
                    validate_scope(row['trace'], row['measurement'])
            budget.check()
        except BaseException as error:
            row.update(status='invalid', cleanup_error=repr(error))
        row['ended_monotonic'] = time.monotonic()
        row['complete_wall_s'] = row['ended_monotonic'] - row['started_monotonic']
        M.save(directory / 'result.json', row)
    M.demand(row['status'] == 'valid', 'S3 sample terminal invalid')
    return row


def probe(root, budget):
    manifest, tool = identity(root)
    directory = M.fresh_directory(root / 'probe')
    payload_root = M.fresh_directory(directory / 'payload')
    payload_file = payload_root / 'probe.bin'
    payload_file.write_bytes(bytes(range(256)) * 4)
    payload = {'name': payload_file.name, 'size': 1024, 'sha256': M.sha(payload_file)}
    row = {'started_monotonic': time.monotonic(), 'status': 'running', 'data_kind': 'capability only; no performance sample', 'strace': tool}
    server = None
    try:
        server = server_start(manifest, payload_root, MatrixRun.MATRIX['M2'], directory, True, budget)
        port = MatrixRun.ready(server, budget)
        server.port = port
        row['trace'] = server.trace
        row['trace']['ready_monotonic'] = time.monotonic()
        row['audit'] = MatrixRun.audit(port, payload, 'keepalive', budget)
        row['trace']['post_audit_completed_monotonic'] = time.monotonic()
        row['status'] = 'valid'
    except BaseException as error:
        row.update(status='invalid', error=repr(error))
        raise
    finally:
        if server:
            try:
                row['server_cleanup'] = server.close(budget.data['deadline_monotonic'])
                row['metrics'] = MatrixRun.parse_metrics(server.stdout_path.read_text())
                validate_scope(row['trace'])
                if row['server_cleanup']['forced']:
                    row['status'] = 'invalid'
            except BaseException as error:
                row.update(status='invalid', cleanup_error=repr(error))
        row['ended_monotonic'] = time.monotonic()
        M.save(directory / 'result.json', row)
    M.demand(row['status'] == 'valid', 'S3 probe terminal invalid')
    return row


def formal(root, budget, sample_function=sample, plan=None):
    plan = SEQUENCE if plan is None else plan
    M.demand(budget.data['phases'].get('probe', {}).get('status') == 'passed', 'probe not passed; no suite')
    manifest, tool = identity(root)
    output = M.fresh_directory(root / 'suite')
    result = {'schema': 2, 'status': 'running', 'samples': [], 'sequence': [{'id': n, 'traced': t} for n, t in plan],
              'strace': tool, 'product_identity': {k: manifest[k] for k in ('commit', 'tree', 'binary_sha256', 'compiler', 'compiler_sha256', 'server_libraries')}}
    result['tool_hashes'] = {p.name: M.sha(p) for p in pathlib.Path(__file__).parent.glob('*.py')}
    payload_root = M.fresh_directory(output / 'payload')
    payloads = {}
    for size in (1024, 1048576):
        path = payload_root / f'payload-{size}.bin'
        path.write_bytes(bytes(range(256)) * (size // 256))
        payloads[size] = {'name': path.name, 'size': size, 'sha256': M.sha(path)}
    try:
        for index, (identifier, traced) in enumerate(plan):
            budget.check()
            directory = M.fresh_directory(output / f"{index + 1:02}-{identifier}-{'traced' if traced else 'untraced'}")
            config = MatrixRun.MATRIX[identifier]
            try:
                row = sample_function(manifest, config, traced, directory, payload_root, payloads[config['size']], budget)
            except BaseException:
                row = json.loads((directory / 'result.json').read_text()) if (directory / 'result.json').exists() else {'status': 'invalid'}
                row.update(index=index + 1, id=identifier)
                result['samples'].append(row)
                M.save(directory / 'result.json', row)
                raise
            row.update(index=index + 1, id=identifier)
            result['samples'].append(row)
            M.save(directory / 'result.json', row)
            M.save(output / 'result.json', result)
            print(f'S3 {index + 1}/{len(plan)} {identifier} traced={traced}: valid', flush=True)
        identity(root)
        M.demand(result['tool_hashes'] == {p.name: M.sha(p) for p in pathlib.Path(__file__).parent.glob('*.py')}, 'analysis tools changed during suite')
        result['status'] = 'valid'
    except BaseException as error:
        result.update(status='invalid', error=repr(error))
        raise
    finally:
        result['counts'] = {'valid': sum(r['status'] == 'valid' for r in result['samples']), 'invalid': sum(r['status'] == 'invalid' for r in result['samples']), 'not_run': len(plan) - len(result['samples'])}
        result['not_run'] = [{'index': i + 1, **row} for i, row in enumerate(result['sequence']) if i >= len(result['samples'])]
        M.save(output / 'result.json', result)
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True)
    parser.add_argument('--mode', choices=('probe', 'formal'), required=True)
    args = parser.parse_args(argv)
    root = role_root(args.root)
    M.demand(root.name != 'rework-002', 'Builder R002 permits fastchecks only; no probe/formal')
    budget = Budget(root)
    budget.begin(args.mode)
    try:
        M.demand(budget.data['phases'].get('fastchecks', {}).get('status') == 'passed', 'fastchecks prerequisite missing')
        result = probe(root, budget) if args.mode == 'probe' else formal(root, budget)
        budget.finish('passed')
        return result
    except BaseException as error:
        budget.finish('invalid', repr(error))
        raise


if __name__ == '__main__':
    main()
