#!/usr/bin/env python3
"""Validate deployment examples and non-root local processes; does not install services."""
import argparse
import configparser
import contextlib
import difflib
import grp
import hashlib
import http.client
import json
import os
from pathlib import Path
import pwd
import queue
import re
import shutil
import signal
import subprocess
import threading
import time

LIMIT = 16 * 1024 * 1024


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_unit(binary, config, work, artifacts):
    source = Path(__file__).resolve().parents[1]
    unit = source / 'examples/systemd/hp-http-server.service'
    original = unit.read_text()
    parsed = configparser.ConfigParser(interpolation=None, strict=True)
    parsed.optionxform = str
    parsed.read_string(original)
    expected = {
        'Unit': {'Description': 'HP HTTP Server (non-privileged example)', 'After': 'network.target',
                 'StartLimitIntervalSec': '60s', 'StartLimitBurst': '3'},
        'Service': {'Type': 'exec', 'User': 'hp-http', 'Group': 'hp-http',
                    'WorkingDirectory': '/srv/hp-http-server',
                    'ExecStart': '/opt/hp-http-server/bin/hp_http_server --config /etc/hp-http-server/server.json',
                    'Restart': 'on-failure', 'RestartSec': '2s', 'RestartPreventExitStatus': '2',
                    'KillSignal': 'SIGTERM', 'KillMode': 'control-group', 'TimeoutStopSec': '15s',
                    'SendSIGKILL': 'no', 'NoNewPrivileges': 'true', 'PrivateTmp': 'true',
                    'ProtectSystem': 'strict', 'ProtectHome': 'true', 'UMask': '0027',
                    'CapabilityBoundingSet': '', 'AmbientCapabilities': '',
                    'StandardOutput': 'journal', 'StandardError': 'journal'},
        'Install': {'WantedBy': 'multi-user.target'}}
    require({section: dict(parsed[section]) for section in parsed.sections()} == expected, 'original unit fields')
    deployed = json.loads((source / 'examples/deployment/server.json').read_text())
    require(deployed == {'port': 8080, 'root': '/srv/hp-http-server/www', 'threads': 2,
                        'idle_timeout_ms': 30000, 'keep_alive_timeout_ms': 15000,
                        'request_read_timeout_ms': 10000, 'shutdown_timeout_ms': 5000,
                        'access_log': False, 'metrics_on_exit': True, 'admin_port': 9090,
                        'max_connections': 128, 'input_budget_bytes': 67108864,
                        'output_budget_bytes': 67108864}, 'paired deployment JSON')
    mappings = {'User': pwd.getpwuid(os.getuid()).pw_name, 'Group': grp.getgrgid(os.getgid()).gr_name,
                'WorkingDirectory': str(work), 'ExecStart': f'{binary} --config {config}'}
    relocated = original
    for key, value in mappings.items():
        require(not any(character.isspace() for character in value) if key != 'ExecStart' else ' ' not in str(binary) + str(config), 'static relocation path without whitespace')
        relocated = relocated.replace(f'{key}={expected["Service"][key]}\n', f'{key}={value}\n', 1)
    copy = artifacts / 'hp-http-server.service'
    copy.write_text(relocated)
    (artifacts / 'unit-relocation.diff').write_text(''.join(difflib.unified_diff(original.splitlines(True), relocated.splitlines(True), fromfile=str(unit), tofile=str(copy))))
    version = subprocess.run(['systemd-analyze', '--version'], capture_output=True, text=True, timeout=5, check=True)
    result = subprocess.run(['systemd-analyze', 'verify', str(copy)], capture_output=True, text=True, timeout=5)
    record = {'original_sha256': digest(unit), 'mapping': mappings, 'version': version.stdout.splitlines()[0],
              'argv': result.args, 'exit': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr,
              'level': 'relocated static syntax only; no systemd execution or hardening verified'}
    (artifacts / 'unit-verify.json').write_text(json.dumps(record, indent=2))
    require(result.returncode == 0 and not result.stdout and not result.stderr, 'unit static verify diagnostics')
    return record


class ServerProcess:
    def __init__(self, binary, config, artifacts, records, fault=None, ports=True):
        self.process = None
        self.readers = []
        self.streams = {'stdout': [], 'stderr': []}
        self.sizes = {'stdout': 0, 'stderr': 0}
        self.reader_errors = []
        self.cleanup_errors = []
        self.events = queue.Queue()
        self.forced = False
        self.cleaned = False
        self.port = self.admin_port = None
        self.record = {'argv': [str(binary), '--config', str(config)], 'fault': fault,
                       'original_error': None, 'pid': None}
        records.append(self.record)
        self.output = artifacts / f'process-{len(records)}'
        try:
            if fault == 'before-popen':
                raise RuntimeError('injected before-popen')
            self.process = subprocess.Popen(self.record['argv'], stdin=subprocess.DEVNULL,
                                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.record['pid'] = self.process.pid
            for name, pipe in [('stdout', self.process.stdout), ('stderr', self.process.stderr)]:
                reader = threading.Thread(target=self.collect, args=(name, pipe))
                self.readers.append(reader)
                reader.start()
            if fault == 'before-port':
                raise RuntimeError('injected before-port')
            if ports:
                business = self.event('V0.1 / S3 minimal HTTP static file server listening on port ')
                self.port = int(re.fullmatch(r'V0.1 / S3 minimal HTTP static file server listening on port ([0-9]+)\.', business).group(1))
                admin = self.event('HP_ADMIN_LISTENING ')
                self.admin_port = int(re.fullmatch(r'HP_ADMIN_LISTENING 127\.0\.0\.1 ([0-9]+)', admin).group(1))
        except BaseException as error:
            self.record['original_error'] = repr(error)
            self.cleanup(error)
            raise

    def collect(self, name, pipe):
        try:
            for raw in iter(lambda: pipe.readline(LIMIT + 1), b''):
                self.sizes[name] += len(raw)
                require(self.sizes[name] <= LIMIT, f'{name} exceeds 16MiB')
                text = raw.decode('utf-8').rstrip('\n')
                self.streams[name].append(text)
                if name == 'stdout':
                    self.events.put(text)
        except BaseException as error:
            self.reader_errors.append(repr(error))
            self.forced = True
            if self.process.poll() is None:
                self.process.kill()

    def event(self, prefix):
        deadline = time.monotonic() + 5
        while True:
            text = self.events.get(timeout=max(0.001, deadline - time.monotonic()))
            if text.startswith(prefix):
                return text
            require(time.monotonic() < deadline, 'stdout control deadline')

    def cleanup(self, original=None):
        if self.cleaned:
            return
        self.cleaned = True
        def attempt(action):
            try:
                action()
            except BaseException as error:
                self.cleanup_errors.append(repr(error))
        if self.process is not None and self.process.poll() is None:
            attempt(lambda: self.process.send_signal(signal.SIGTERM))
            try:
                self.process.wait(timeout=10)
            except BaseException as error:
                self.cleanup_errors.append(repr(error))
                self.forced = True
                attempt(self.process.kill)
                attempt(lambda: self.process.wait(timeout=5))
        if self.process is not None:
            attempt(lambda: self.process.wait(timeout=5))
        for reader in self.readers:
            attempt(lambda: reader.join(timeout=5))
        if self.process is not None:
            for pipe in (self.process.stdout, self.process.stderr):
                attempt(pipe.close)
        for name, lines in self.streams.items():
            attempt(lambda name=name, lines=lines: self.output.with_suffix('.' + name).write_text('\n'.join(lines)))
        self.record.update(exit=self.process.returncode if self.process else None,
                           reaped=self.process is None or self.process.returncode is not None,
                           readers_joined=all(not reader.is_alive() for reader in self.readers),
                           pipes_closed=self.process is None or (self.process.stdout.closed and self.process.stderr.closed),
                           pid_exists=self.process is not None and Path(f'/proc/{self.process.pid}').exists(),
                           forced=self.forced, reader_errors=self.reader_errors, cleanup_errors=self.cleanup_errors,
                           port=self.port, admin_port=self.admin_port, stdout_bytes=self.sizes['stdout'], stderr_bytes=self.sizes['stderr'])
        clean = self.record['reaped'] and self.record['readers_joined'] and self.record['pipes_closed'] and not self.record['pid_exists'] and not self.forced and not self.reader_errors and not self.cleanup_errors
        if not clean:
            if original:
                original.add_note('cleanup failure: ' + repr(self.record))
            else:
                raise AssertionError('cleanup failure: ' + repr(self.record))

    def __enter__(self):
        return self

    def __exit__(self, kind, error, traceback):
        if error:
            self.record['original_error'] = repr(error)
        self.cleanup(error)
        if error is None:
            require(self.record['exit'] == 0, 'normal process exit')

    def metrics(self):
        text = '\n'.join(self.streams['stdout'])
        require(text.count('HP_METRICS_BEGIN') == 1 and text.count('HP_METRICS_END') == 1, 'complete exit metrics')
        body = text.split('HP_METRICS_BEGIN\n', 1)[1].split('HP_METRICS_END', 1)[0]
        return {key: int(value) for key, value in (line.split() for line in body.splitlines())}


def exchange(port, method, target, body=None, status=200):
    encoded = json.dumps(body).encode() if body is not None else None
    with contextlib.closing(http.client.HTTPConnection('127.0.0.1', port, timeout=5)) as client:
        client.request(method, target, body=encoded, headers={'Connection': 'close', **({'Content-Type': 'application/json'} if encoded else {})})
        response = client.getresponse()
        payload = response.read(262145)
        require(response.status == status, f'{method} {target} expected {status}, got {response.status}')
        require(len(payload) <= 262144, 'response bound')
        require((method == 'HEAD' and not payload) or int(response.getheader('Content-Length')) == len(payload), 'response length')
        return payload


def assert_clean(records):
    for record in records:
        require(record['reaped'] and record['readers_joined'] and record['pipes_closed'] and not record['pid_exists'] and not record['forced'] and not record['reader_errors'] and not record['cleanup_errors'], 'owned process cleanup')


def run(binary, artifacts, negative):
    require(os.getuid() != 0, 'BLOCKED: permission checks require non-root UID')
    require(binary.is_absolute() and binary.is_file() and os.access(binary, os.X_OK), '--server must be an absolute executable file')
    require(artifacts.is_absolute() and artifacts.parent.is_dir() and not artifacts.exists(), '--artifacts requires an absent absolute directory with existing parent')
    owned_artifacts = owned_work = owned_root = False
    errors = []
    work = artifacts / 'work'
    root = work / 'www'
    records, checks = [], []
    original = None
    summary = {'passed': False, 'uid': None, 'pid1': None,
               'server': str(binary), 'server_sha256': None, 'records': records, 'checks': checks,
               'listen_address': '0.0.0.0', 'admin_address': '127.0.0.1', 'forced': False}
    page = root / 'index.html'
    config = work / 'server.json'
    try:
        artifacts.mkdir(mode=0o700)
        owned_artifacts = True
        work.mkdir(mode=0o700)
        owned_work = True
        root.mkdir(mode=0o700)
        owned_root = True
        summary['uid'] = os.getuid()
        summary['pid1'] = Path('/proc/1/comm').read_text().strip()
        summary['server_sha256'] = digest(binary)
        page.write_bytes(b'deployment fixture\n'); page.chmod(0o600)
        config.write_text(json.dumps({'port': 0, 'root': str(root), 'threads': 2, 'admin_port': 0,
                                      'metrics_on_exit': True, 'shutdown_timeout_ms': 1000})); config.chmod(0o600)
        summary['unit'] = verify_unit(binary, config, work, artifacts)
        for fault in ('before-popen', 'before-port', 'after-http'):
            caught = None
            try:
                with ServerProcess(binary, config, artifacts, records, fault) as server:
                    exchange(server.admin_port, 'GET', '/healthz')
                    raise RuntimeError('injected after-http')
            except RuntimeError as error:
                caught = error
            require(caught is not None and str(caught) == 'injected ' + fault, 'original injected failure preserved')
            assert_clean(records)
            checks.append('fault:' + fault)
        bad = work / 'bad.json'; bad.write_text('{"unknown":true}'); bad.chmod(0o600)
        for name, path in [('bad-config', bad), ('root-denied', config)]:
            if name == 'root-denied': root.chmod(0)
            try:
                failed = ServerProcess(binary, path, artifacts, records, ports=False)
                try:
                    failed.process.wait(timeout=5)
                finally:
                    failed.cleanup()
                require(failed.record['exit'] == (2 if name == 'bad-config' else 1), name + ' exit code')
                require(not any('listening on port ' in line or line.startswith('HP_ADMIN_LISTENING ') for line in failed.streams['stdout']), name + ' must have no ready declaration')
            finally:
                root.chmod(0o700)
            checks.append(name)
        with ServerProcess(binary, config, artifacts, records) as first:
            require(exchange(first.admin_port, 'GET', '/healthz') == b'ok\n', 'health')
            require(exchange(first.admin_port, 'GET', '/readyz') == b'ready\n', 'ready')
            metrics = exchange(first.admin_port, 'GET', '/metrics').decode()
            require('resource_connections_current ' in metrics and 'HP_METRICS_' not in metrics, 'live metrics')
            require(exchange(first.port, 'GET', '/') == b'deployment fixture\n', 'static GET')
            exchange(first.port, 'HEAD', '/')
            page.chmod(0)
            try:
                exchange(first.port, 'GET', '/', status=403)
            finally:
                page.chmod(0o600)
            created = json.loads(exchange(first.port, 'POST', '/api/tasks', {'title': 'ephemeral', 'completed': False}, 201))
            require(json.loads(exchange(first.port, 'GET', '/api/tasks')) == {'tasks': [created]}, 'created task listed')
        final = first.metrics()
        require(all(final[f'resource_{key}_current'] == 0 for key in ('connections', 'input_bytes', 'output_bytes')), 'first exit resources')
        checks.append('first normal SIGTERM/metrics/static403/task')
        with ServerProcess(binary, config, artifacts, records) as second:
            require(json.loads(exchange(second.port, 'GET', '/api/tasks')) == {'tasks': []}, 'restart clears tasks')
            require(exchange(second.admin_port, 'GET', '/readyz') == b'ready\n', 'restart ready')
        second.metrics()
        checks.append('second normal SIGTERM/restart empty')
        assert_clean(records)
        require(not negative, 'intentional deployment wrong expectation')
        summary['passed'] = True
    except BaseException as error:
        original = error
        summary['original_error'] = repr(error)
        raise
    finally:
        # Each operation is independent: a permission restoration failure must not
        # prevent removal, and reporting failures must not replace the cause.
        for action in ([lambda: root.chmod(0o700)] if owned_root else []) + (
                [lambda: page.chmod(0o600) if page.exists() else None,
                 lambda: shutil.rmtree(work)] if owned_work else []):
            try:
                action()
            except BaseException as error:
                errors.append(repr(error))
        try:
            removed = not owned_work or not work.exists()
            summary.update(root_removed=removed, permission_restored=removed, cleanup_errors=errors,
                           forced=any(record.get('forced', False) for record in records))
            if errors or summary['forced']:
                summary['passed'] = False
            if owned_artifacts:
                (artifacts / 'summary.json').write_text(json.dumps(summary, indent=2))
                print('DEPLOYMENT_SUMMARY', artifacts / 'summary.json', flush=True)
        except BaseException as error:
            errors.append('summary: ' + repr(error))
        if errors:
            if original is not None:
                original.add_note('deployment cleanup/reporting: ' + repr(errors))
            else:
                raise RuntimeError('deployment cleanup/reporting: ' + repr(errors))
    return summary


def interrupted(signum, frame):
    raise RuntimeError(f'deployment interrupted by signal {signum}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', required=True, type=Path)
    parser.add_argument('--artifacts', required=True, type=Path)
    parser.add_argument('--negative', action='store_true')
    args = parser.parse_args()
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGALRM, interrupted)
    signal.alarm(120)
    try:
        run(args.server, args.artifacts, args.negative)
    finally:
        signal.alarm(0)


if __name__ == '__main__':
    main()
