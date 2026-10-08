#!/usr/bin/env python3
"""V0.6/S2 fixed identities, finite role budget and owned child lifecycle.

Uses the proven archive/hash and /proc identity methods of ../build.py/run.py;
this independent entry does not import or mutate the historical A/B runner.
"""
import hashlib
import json
import os
import pathlib
import resource
import shutil
import signal
import subprocess
import time

REPO = pathlib.Path(__file__).resolve().parents[2]
COMMIT = '1340f5bb8a303769a8be32b2d8cc29482fd2a8fe'
TREE = 'f2fdabf35fb754b7f4e981708efd34e40e53ceac'
WRK = REPO / '.cache/v0.5-s4/tools/root/usr/bin/wrk'
LIB = REPO / '.cache/v0.5-s4/tools/root/usr/lib/x86_64-linux-gnu'
WRK_SHA = 'b10e53769443c2bf3be2cdedec8ef6571aa5bfd1494796b247f3f9296e3af71d'
FLAGS = ['-DCMAKE_BUILD_TYPE=Release', '-DBUILD_TESTING=OFF',
         '-DCMAKE_CXX_COMPILER=/usr/bin/g++', '-DCMAKE_CXX_STANDARD=20',
         '-DCMAKE_CXX_FLAGS=', '-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG',
         '-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON']
RECOVERY_APPROVALS = {
    'rework-001': 'e1ccb058e5d6f79f4ed66bd2d9bf1882155df051d8a7820492e5ad9b3b5f1131',
    'rework-002': '7c90b0f62295b30c4ec91454e34d40e19c8a64b2c4851d16ce32d84dde5b6f29'}
CAPS = {'build': 600, 'fastchecks': 120, 'smoke': 60, 'formal': 600}


def demand(condition, message):
    if not condition:
        raise ValueError(message)


def save(path, value):
    pathlib.Path(path).write_text(json.dumps(value, indent=2, allow_nan=False) + '\n')


def sha(path):
    digest = hashlib.sha256()
    with pathlib.Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1048576), b''):
            digest.update(block)
    return digest.hexdigest()


def role_root(path):
    root = pathlib.Path(path).resolve()
    demand(root.parent == REPO / '.cache/v0.6-s2' and root.name in ('builder', 'reviewer'), 'role root mismatch')
    demand(root.is_dir(), 'role root must exist; controller owns future children')
    return root


def fresh_directory(path):
    path = pathlib.Path(path)
    demand(not path.exists(), 'output already exists; no overwrite or retry')
    path.mkdir()
    return path


def sizes(root):
    logical = allocated = raw = 0
    for path in pathlib.Path(root).rglob('*'):
        if path.is_file():
            stat = path.stat()
            logical += stat.st_size
            allocated += stat.st_blocks * 512
            if path.suffix in ('.log', '.stdout', '.stderr'):
                raw += stat.st_size
    return {'logical_bytes': logical, 'allocated_bytes': allocated, 'charged_bytes': max(logical, allocated), 'raw_bytes': raw}


class Budget:
    def __init__(self, root, create=False, recovery=False):
        self.recovery = recovery
        self.recovery_name = recovery if isinstance(recovery, str) else 'rework-001'
        if recovery:
            demand(self.recovery_name in RECOVERY_APPROVALS, 'unknown recovery name')
        self.root = pathlib.Path(root)
        self.output_root = self.root / self.recovery_name if recovery else self.root
        self.path = self.output_root / 'budget.json'
        if create and recovery:
            demand(self.root.name == 'builder', 'recovery is Builder only')
            fresh_directory(self.output_root)
        if create:
            demand(not self.path.exists(), 'dynamic budget already started')
            now = time.monotonic()
            disk = shutil.disk_usage(self.root)
            soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
            demand(disk.free >= 4 * 1024**3, 'available disk cannot hold approved role limit')
            demand(soft == resource.RLIM_INFINITY or soft >= 160, 'nofile below 128 connections + 32 control/thread reserve')
            self.data = {'schema': 1, 'started_monotonic': now, 'deadline_monotonic': now + (600 if recovery else 1200),
                         'started_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                         'boot_id': pathlib.Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
                         'disk_free_before': disk.free, 'nofile': [soft, hard], 'required_fds_per_process': 160,
                         'phases': {}, 'limits': {'total_s': 600 if recovery else 1200, 'cleanup_s': 10, 'raw_bytes': 512 * 1024**2, 'task_bytes': 4 * 1024**3}}
            if recovery:
                self.data['recovery'] = {
                    'name': 'S2-' + self.recovery_name, 'approval': 'PM explicit ' + self.recovery_name + ' approval',
                    'rework_sha256': sha(REPO / ('docs/leader/reworks/V0.6/S2-' + self.recovery_name + '.md')),
                    'old_budget_sha256': sha(self.root / 'budget.json'),
                    'old_settlement_sha256': sha(self.root / 'budget-settlement.json'),
                    'role_root': str(self.root), 'host': os.uname().nodename}
                if self.recovery_name == 'rework-002':
                    self.data['recovery']['r001_ledgers'] = {name: sha(self.root / 'rework-001' / name) for name in ('budget.json', 'budget-settlement.json')}
                self.data['usage_before'] = sizes(self.root)
            self.write()
        else:
            self.data = json.loads(self.path.read_text())
            demand(self.data['boot_id'] == pathlib.Path('/proc/sys/kernel/random/boot_id').read_text().strip(), 'monotonic host changed')
        if recovery:
            reference = self.data['recovery']
            demand(reference['name'] == 'S2-' + self.recovery_name and reference['role_root'] == str(self.root) and reference['host'] == os.uname().nodename, 'recovery identity mismatch')
            demand(reference['old_budget_sha256'] == sha(self.root / 'budget.json') and reference['old_settlement_sha256'] == sha(self.root / 'budget-settlement.json'), 'old budget changed')
            demand(reference['rework_sha256'] == RECOVERY_APPROVALS[self.recovery_name], 'recovery approval mismatch')
            if self.recovery_name == 'rework-002':
                demand(reference['r001_ledgers'] == {name: sha(self.root / 'rework-001' / name) for name in ('budget.json', 'budget-settlement.json')}, 'R001 ledgers changed')
        self.last_scan = 0
        self.phase = None
        self.phase_deadline = self.data['deadline_monotonic'] - 10

    def write(self):
        self.data['elapsed_s'] = time.monotonic() - self.data['started_monotonic']
        self.data['remaining_s'] = max(0, self.data['deadline_monotonic'] - time.monotonic())
        save(self.path, self.data)

    def begin(self, phase):
        demand(phase not in self.data['phases'], 'phase already consumed; no retry')
        self.check()
        self.phase = phase
        now = time.monotonic()
        self.phase_deadline = min(self.data['deadline_monotonic'] - 10, now + (60 if self.recovery and phase == 'fastchecks' else CAPS[phase]))
        self.data['phases'][phase] = {'status': 'running', 'start_monotonic': now, 'deadline_monotonic': self.phase_deadline}
        self.write()

    def check(self, deadline=None):
        now = time.monotonic()
        demand(now < min(self.phase_deadline, deadline if deadline is not None else self.phase_deadline), 'absolute deadline exhausted')
        if now - self.last_scan >= 1:
            self.data['usage'] = sizes(self.root)
            self.last_scan = now
        usage = self.data['usage']
        demand(usage['raw_bytes'] <= self.data['limits']['raw_bytes'], 'raw output limit exceeded (detective limit)')
        demand(usage['charged_bytes'] <= self.data['limits']['task_bytes'], 'task space limit exceeded (detective limit)')

    def finish(self, status, error=None):
        now = time.monotonic()
        if self.phase:
            row = self.data['phases'][self.phase]
            row.update(status=status, end_monotonic=now, elapsed_s=now - row['start_monotonic'], error=error)
        self.data['usage'] = sizes(self.root)
        self.write()


def process_info(pid):
    base = pathlib.Path('/proc') / str(pid)
    fields = (base / 'stat').read_text().rsplit(')', 1)[1].split()
    status = dict(line.split(':', 1) for line in (base / 'status').read_text().splitlines() if ':' in line)
    ticks_per_second = os.sysconf('SC_CLK_TCK')
    return {'pid': pid, 'starttime': int(fields[19]),
            'utime_ticks': int(fields[11]), 'stime_ticks': int(fields[12]),
            'clock_ticks_per_second': ticks_per_second, 'read_monotonic': time.monotonic(),
            'cpu_seconds': (int(fields[11]) + int(fields[12])) / ticks_per_second,
            'rss_kib': int(status.get('VmRSS', '0 kB').split()[0]),
            'hwm_kib': int(status.get('VmHWM', '0 kB').split()[0])}


class OwnedProcess:
    def __init__(self, command, prefix, env=None):
        self.command = command
        self.prefix = pathlib.Path(prefix)
        self.stdout_path = self.prefix.with_suffix('.stdout')
        self.stderr_path = self.prefix.with_suffix('.stderr')
        self.stdout = self.stdout_path.open('wb')
        self.stderr = self.stderr_path.open('wb')
        self.forced = False
        self.usage = None
        self.wait4_pid = None
        self.wait4_read_monotonic = None
        self.process = subprocess.Popen(command, stdout=self.stdout, stderr=self.stderr, env=env, start_new_session=True)
        try:
            self.identity = process_info(self.process.pid)
            save(self.prefix.with_suffix('.process.json'), {'argv': command, **self.identity})
        except BaseException:
            self.process.kill()
            self.process.wait(timeout=3)
            self.stdout.close()
            self.stderr.close()
            raise

    def poll(self):
        if self.process.returncode is None:
            pid, status, usage = os.wait4(self.process.pid, os.WNOHANG)
            if pid:
                self.process.returncode = os.waitstatus_to_exitcode(status)
                self.usage = usage
                self.wait4_pid = pid
                self.wait4_read_monotonic = time.monotonic()
        return self.process.returncode

    def matches(self):
        try:
            return process_info(self.process.pid)['starttime'] == self.identity['starttime'] and os.getpgid(self.process.pid) == self.process.pid
        except ProcessLookupError:
            return False
        except FileNotFoundError:
            return False

    def close(self, deadline=None):
        started = time.monotonic()
        deadline = min(deadline if deadline is not None else started + 10, started + 10)
        if self.poll() is None:
            demand(self.matches(), 'PID/starttime/process-group mismatch; refusing signal')
            os.killpg(self.process.pid, signal.SIGTERM)
            while self.poll() is None and time.monotonic() < min(started + 7, deadline - 3):
                time.sleep(0.01)
            if self.poll() is None:
                self.forced = True
                demand(self.matches(), 'PID identity changed before kill')
                os.killpg(self.process.pid, signal.SIGKILL)
                while self.poll() is None and time.monotonic() < deadline:
                    time.sleep(0.01)
        self.stdout.close()
        self.stderr.close()
        result = {'pid': self.identity['pid'], 'starttime': self.identity['starttime'], 'returncode': self.poll(),
                  'forced': self.forced, 'reaped': self.process.returncode is not None,
                  'cleanup_elapsed_s': time.monotonic() - started}
        if self.usage is not None:
            result['wait4'] = {'pid': self.wait4_pid, 'ru_utime_s': self.usage.ru_utime, 'ru_stime_s': self.usage.ru_stime, 'read_monotonic': self.wait4_read_monotonic}
        save(self.prefix.with_suffix('.cleanup.json'), result)
        demand(result['reaped'], 'owned child not reaped within cleanup allowance')
        return result


def wait_process(child, budget, deadline):
    while child.poll() is None:
        budget.check(deadline)
        time.sleep(0.05)
    return child.process.returncode
