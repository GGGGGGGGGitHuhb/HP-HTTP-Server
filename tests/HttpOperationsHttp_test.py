"""V1.4/S2 bounded configuration and real independent management HTTP acceptance."""
import contextlib
import json
import os
from pathlib import Path
import queue
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

from HttpResourceBudgetHttp_test import BudgetServer, ScheduledPipe, require, read_metrics, interrupt_suite


class OperationsServer(BudgetServer):
    def __init__(self, binary, root, logs, options=(), admin=True, schedule=False):
        self.process = self.reader = self.stderr_reader = None
        self.log_lock = threading.Lock()
        self.log_size = 0
        self.port = self.admin_port = None
        self.mode = 'main'
        self.events = queue.Queue()
        self.lines = []
        self.stdout_lines = []
        self.reader_error = None
        self.cleanup_errors = []
        self.cleaned = self.forced = False
        self.original_error = None
        self.schedule = schedule
        self.release_stderr = threading.Event()
        self.stderr_appended = threading.Event()
        self.log_path = logs / f'operations-{time.monotonic_ns()}.log'
        self.command = [binary, '--port', '0', '--root', str(root), '--threads', '0', '--metrics-on-exit']
        if admin:
            self.command += ['--admin-port', '0']
        self.command += list(options)
        try:
            self.process = subprocess.Popen(self.command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.reader = threading.Thread(target=self.collect_output)
            self.stderr_reader = threading.Thread(target=self.collect_stderr)
            self.reader.start()
            self.stderr_reader.start()
            self.port = self.wait_port()
            if admin:
                line = self.event('HP_ADMIN_LISTENING ')
                require(line.startswith('HP_ADMIN_LISTENING 127.0.0.1 '), 'admin address announcement')
                self.admin_port = int(line.split()[2])
        except BaseException as failure:
            self.original_error = repr(failure)
            self.cleanup()
            raise

    def collect_stream(self, stream, protocol):
        if self.schedule:
            stream = ScheduledPipe(stream, protocol, self.release_stderr, self.stderr_appended)
        super().collect_stream(stream, protocol)


def connect(port):
    client = socket.create_connection(('127.0.0.1', port), timeout=5)
    client.settimeout(5)
    return client


def exchange(port, target, method='GET', body=b'', extra=b''):
    with contextlib.closing(connect(port)) as client:
        client.sendall(method.encode() + b' ' + target.encode() + b' HTTP/1.1\r\nHost: localhost\r\nContent-Length: ' + str(len(body)).encode() + b'\r\nConnection: close\r\n' + extra + b'\r\n' + body)
        response = b''
        while True:
            block = client.recv(4096)
            if not block:
                break
            response += block
            require(len(response) <= 65536, 'bounded response')
    head, content = response.split(b'\r\n\r\n', 1)
    fields = head.split(b'\r\n')
    status = int(fields[0].split()[1])
    headers = [line.lower() for line in fields[1:]]
    return status, headers, content


def live_metrics(port):
    status, _, body = exchange(port, '/metrics')
    require(status == 200 and b'HP_METRICS_' not in body, 'live metrics envelope')
    return {key: int(value) for key, value in (line.split() for line in body.decode().splitlines())}


def wait_snapshot(port, predicate):
    deadline = time.monotonic() + 5
    for _ in range(1000):
        value = live_metrics(port)
        if predicate(value):
            return value
        require(time.monotonic() < deadline, 'snapshot deadline')
    raise AssertionError('snapshot operation bound')


def check_routes(binary, root, logs):
    with OperationsServer(binary, root, logs, schedule=True) as server:
        # /proc records the actual listener address, rather than the client's chosen destination.
        rows = Path('/proc/net/tcp').read_text().splitlines()[1:]
        require(any(row.split()[1] == f'0100007F:{server.admin_port:04X}' and row.split()[3] == '0A' for row in rows), 'actual loopback listener')
        for target, method, body, expected in [('/healthz', 'GET', b'', 200), ('/readyz', 'GET', b'', 200), ('/metrics', 'GET', b'', 200), ('/healthz?x', 'GET', b'', 404), ('/%68ealthz', 'GET', b'', 404), ('/unknown', 'POST', b'', 404), ('/healthz', 'POST', b'', 405), ('/healthz', 'HEAD', b'', 405), ('/healthz', 'GET', b'x', 400)]:
            status, headers, content = exchange(server.admin_port, target, method, body)
            require(status == expected, 'route status ' + target)
            require(headers.count(b'cache-control: no-store') == 1 and b'content-type: text/plain' in headers, 'management response headers')
            if method == 'HEAD':
                require(content == b'' and b'allow: get' in headers, 'HEAD405')
        status, _, _ = exchange(server.admin_port, '/healthz', extra=b'Expect: 100-continue\r\n', body=b'x')
        require(status == 417, 'Expect rejects before100')
        require(exchange(server.port, '/healthz')[0] == 404, 'business route isolated')
        before = wait_snapshot(server.admin_port, lambda v: v['connections_active'] == 0)
        for _ in range(4):
            after = live_metrics(server.admin_port)
        require(before['requests_started_total'] == after['requests_started_total'], 'management feedback')
        sockets = []
        try:
            for _ in range(8):
                sockets.append(connect(server.admin_port))
            with contextlib.closing(connect(server.admin_port)) as rejected:
                try:
                    require(rejected.recv(1) == b'', 'ninth admin connection denied')
                except ConnectionResetError:
                    pass
        finally:
            for client in sockets:
                client.close()
    mixed = server.log_path.read_text().split('HP_METRICS_BEGIN\n', 1)[1].split('HP_METRICS_END', 1)[0]
    require('STDERR ' in mixed, 'real delayed stderr schedule')
    final = read_metrics(server)
    require(len([key for key in final if key.startswith('resource_')]) == 9, 'stdout metrics source')
    print('CASE O02 actual loopback/routes/budget8/metrics isolation/real pipe schedule PASS', flush=True)


def check_saturation_and_drain(binary, root, logs):
    with OperationsServer(binary, root, logs, ['--max-connections', '1', '--shutdown-timeout-ms', '5000']) as server:
        with contextlib.closing(connect(server.port)) as held:
            wait_snapshot(server.admin_port, lambda v: v['resource_connections_current'] == 1)
            require(exchange(server.admin_port, '/readyz')[2] == b'ready\n', 'business full must not block admin')
            with contextlib.closing(connect(server.port)) as rejected:
                try:
                    require(rejected.recv(1) == b'', 'business admission full')
                except ConnectionResetError:
                    pass
            held.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            held.sendall(b'GET /large.bin HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n')
            wait_snapshot(server.admin_port, lambda v: v['resource_output_bytes_current'] > 1024 * 1024)
            server.process.send_signal(signal.SIGTERM)
            deadline = time.monotonic() + 5
            for _ in range(1000):
                response = exchange(server.admin_port, '/readyz')
                if response[0] == 503:
                    break
                require(time.monotonic() < deadline, 'not ready deadline')
            else:
                raise AssertionError('readiness operation bound')
            require(response[2] == b'not ready\n', 'drain readiness')
            server.process.send_signal(signal.SIGTERM)
    print('CASE O03 business full/slow output/admin responsive/drain not ready PASS', flush=True)


def check_config(binary, root, logs):
    config = logs / 'config.json'
    valid = {'port': 0, 'root': str(root), 'threads': 2, 'admin_port': 0, 'access_log': True}
    config.write_text(json.dumps(valid))
    with OperationsServer(binary, root, logs, ['--config', str(config), '--no-access-log']) as server:
        require(exchange(server.admin_port, '/healthz')[2] == b'ok\n', 'file settings')
    with OperationsServer(binary, root, logs, ['--config', str(config), '--no-admin'], admin=False) as server:
        require(exchange(server.port, '/')[0] == 200, 'CLI disables file admin')
    cases = [b'{}', b'[]', b'{"port":0,"port":1}', b'{"unknown":1}', b'{"threads":-1}', b'{"threads":1.0}', b'{"threads":null}', b'{"threads":65}', b'{"threads":18446744073709551616}', b'{"root":"\xff"}', b'\xef\xbb\xbf{}', b'{} trailing', b'{"port":{"x":1}}', b'{"port":[1]}', b'{"access_log":1}']
    records = []
    for data in cases:
        config.write_bytes(data)
        arguments = [binary, '--config', str(config)]
        if data != b'{}':
            arguments += ['--port', '0', '--root', str(root)]
        result = subprocess.run(arguments, capture_output=True, timeout=5)
        require(result.returncode == 2 and not result.stdout, 'bad config must exit2 before startup')
        records.append({'input_hex': data.hex(), 'exit': result.returncode})
    for path in (root, logs / 'fifo'):
        if path.name == 'fifo':
            os.mkfifo(path)
        result = subprocess.run([binary, '--config', str(path)], capture_output=True, timeout=5)
        require(result.returncode == 2 and not result.stdout, 'nonregular config')
    config.write_text(json.dumps(valid).ljust(65536))
    with OperationsServer(binary, root, logs, ['--config', str(config)]) as server:
        require(exchange(server.admin_port, '/healthz')[0] == 200, '64KiB exact allowed')
    with config.open('a') as output:
        output.write(' ')
    result = subprocess.run([binary, '--config', str(config)], capture_output=True, timeout=5)
    require(result.returncode == 2 and not result.stdout, '64KiB plus1 rejects')
    for extra in [['--access-log', '--no-access-log'], ['--admin-port', '0', '--no-admin'], ['--threads', '0', '--threads', '1'], ['--root', '']]:
        result = subprocess.run([binary, '--port', '0', '--root', str(root)] + extra, capture_output=True, timeout=5)
        require(result.returncode == 2 and not result.stdout, 'duplicate/invalid CLI')
    with contextlib.closing(socket.socket()) as occupied:
        occupied.bind(('127.0.0.1', 0)); occupied.listen()
        result = subprocess.run([binary, '--port', '0', '--root', str(root), '--admin-port', str(occupied.getsockname()[1])], capture_output=True, timeout=5)
        require(result.returncode == 1 and not result.stdout, 'admin bind failure no half startup')
    result = subprocess.run([binary, '--port', '0', '--root', str(root / 'absent')], capture_output=True, timeout=5)
    require(result.returncode == 1 and not result.stdout, 'root runtime failure')
    (logs / 'config-results.json').write_text(json.dumps(records, indent=2))
    print('CASE O01 config source/strict invalid/boundaries/nonregular/runtime failure PASS', flush=True)


def check_cleanup(binary, root, logs):
    for phase in ('before-port', 'after-port', 'running'):
        marker = RuntimeError('operations deliberate ' + phase)
        class FailingServer(OperationsServer):
            def wait_port(self):
                if phase == 'before-port':
                    raise marker
                port = super().wait_port()
                if phase == 'after-port':
                    raise marker
                return port
        observed = None
        try:
            with FailingServer(binary, root, logs) as server:
                exchange(server.admin_port, '/healthz')
                raise marker
        except RuntimeError as failure:
            observed = failure
        require(observed is marker, 'original cleanup exception')
    print('CASE O05 before/after PORT/running own cleanup PASS', flush=True)


def main():
    binary = sys.argv[1]
    base = Path(os.environ['HP_S3_TEST_TMP_ROOT'])
    base.mkdir(parents=True, exist_ok=True)
    logs = Path(tempfile.mkdtemp(prefix='operations-evidence-', dir=base))
    root = Path(tempfile.mkdtemp(prefix='operations-static-', dir=base))
    passed = False
    original = None
    signal.signal(signal.SIGTERM, interrupt_suite)
    signal.signal(signal.SIGALRM, interrupt_suite)
    signal.alarm(120)
    try:
        (root / 'index.html').write_bytes(b'home')
        with (root / 'large.bin').open('wb') as output:
            output.truncate(8 * 1024 * 1024)
        check_cleanup(binary, root, logs)
        check_config(binary, root, logs)
        check_routes(binary, root, logs)
        check_saturation_and_drain(binary, root, logs)
        require(not os.environ.get('HP_OPERATIONS_NEGATIVE'), 'intentional operations wrong expectation')
        passed = True
    except BaseException as failure:
        original = failure
        raise
    finally:
        errors = []
        try:
            shutil.rmtree(root)
        except BaseException as failure:
            errors.append(repr(failure))
        signal.alarm(0)
        (logs / 'summary.json').write_text(json.dumps({'passed': passed and not errors, 'root_removed': not root.exists(), 'original_error': repr(original) if original else None, 'cleanup_errors': errors}, indent=2))
        if errors:
            if original:
                original.add_note(repr(errors))
            else:
                raise RuntimeError(repr(errors))
        print('OPERATIONS_SUMMARY', logs / 'summary.json', flush=True)


if __name__ == '__main__':
    main()
