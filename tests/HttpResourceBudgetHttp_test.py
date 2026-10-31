#!/usr/bin/env python3
"""跨owner逻辑预算：真实准入/过载/恢复与自身清理。"""
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

from HttpProtocolBaselineHttp_test import Client, request, require, interrupt_suite
from HttpTaskApiHttp_test import TaskClient, task_wire, encoded, exchange

WINDOW = 16 * 1024
REQUEST = 1040 * 1024
MIB = 1024 * 1024
FIELDS = ('connections', 'input', 'output', 'peak_connections', 'peak_input', 'peak_output',
          'reject_connections', 'reject_input', 'reject_output', 'jobs', 'admitted', 'released',
          'started', 'completed', 'aborted', 'parse_errors', 'provider_errors')
NOTICE = '测试监听0.0.0.0临时端口，客户端127.0.0.1；仅使用可信开发环境内临时合成数据。'


class BudgetServer:
    def __init__(self, binary, root, threads, logs, mode):
        self.process = None
        self.reader = None
        self.stderr_reader = None
        self.log_lock = threading.Lock()
        self.log_size = 0
        self.port = None
        self.mode = mode
        self.events = queue.Queue()
        self.lines = []
        self.stdout_lines = []
        self.reader_error = None
        self.cleanup_errors = []
        self.cleaned = False
        self.forced = False
        self.original_error = None
        self.log_path = logs / f'budget-{time.monotonic_ns()}.log'
        self.command = ([binary, '--port', '0', '--root', str(root), '--threads', str(threads), '--metrics-on-exit']
                        if mode == 'main' else [binary, str(root), str(threads), mode])
        print(NOTICE, flush=True)
        try:
            self.process = subprocess.Popen(self.command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                            stderr=subprocess.PIPE)
            self.reader = threading.Thread(target=self.collect_output, name='resource-fixture-output')
            self.reader.start()
            self.stderr_reader = threading.Thread(target=self.collect_stderr, name="resource-fixture-errors")
            self.stderr_reader.start()
            self.port = self.wait_port()
        except BaseException as failure:
            self.original_error = repr(failure)
            self.cleanup()
            if self.cleanup_errors:
                failure.add_note('cleanup: ' + repr(self.cleanup_errors))
            raise

    def collect_output(self):
        self.collect_stream(self.process.stdout, True)

    def collect_stderr(self):
        self.collect_stream(self.process.stderr, False)

    def collect_stream(self, stream, protocol):
        try:
            for raw in iter(lambda: stream.readline(16 * MIB + 1), b''):
                with self.log_lock:
                    self.log_size += len(raw)
                    if self.log_size > 16 * MIB:
                        raise RuntimeError('fixture log exceeds 16MiB')
                    line = raw.decode(errors='replace').rstrip()
                    self.lines.append(line if protocol else 'STDERR ' + line)
                    if protocol:
                        self.stdout_lines.append(line)
                if protocol:
                    self.events.put(line)
        except BaseException as failure:
            self.reader_error = repr(failure)
            if self.process.poll() is None:
                self.process.kill()

    def event(self, prefix):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            line = self.events.get(timeout=max(0.001, deadline - time.monotonic()))
            if line.startswith(prefix):
                return line
        raise TimeoutError('fixture event ' + prefix)

    def wait_port(self):
        if self.mode == 'main':
            line = self.event('V0.1 / S3 minimal HTTP static file server listening on port ')
            return int(re.search(r'port ([0-9]+)\.', line).group(1))
        return int(self.event('PORT ').split()[1])

    def command_line(self, command):
        self.process.stdin.write(command.encode() + b'\n')
        self.process.stdin.flush()

    def snapshot(self):
        self.command_line('snapshot')
        fields = self.event('SNAP ').split()[1:]
        require(len(fields) == len(FIELDS), 'snapshot schema')
        return dict(zip(FIELDS, map(int, fields)))

    def wait_state(self, predicate):
        deadline = time.monotonic() + 5
        for _ in range(500):
            state = self.snapshot()
            if predicate(state):
                return state
            require(time.monotonic() < deadline, 'state deadline')
        raise AssertionError('bounded snapshot attempts exhausted: ' + repr(state))

    def cleanup(self):
        if self.cleaned:
            return self.record

        def attempt(action):
            try:
                action()
            except BaseException as failure:
                self.cleanup_errors.append(repr(failure))

        process = self.process
        if process is not None:
            if process.poll() is None:
                attempt(lambda: process.send_signal(signal.SIGTERM))
                if self.mode != 'main':
                    attempt(lambda: self.command_line('quit'))
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.forced = True
                attempt(process.kill)
                attempt(lambda: process.wait(timeout=5))
            except BaseException as failure:
                self.cleanup_errors.append(repr(failure))
                attempt(process.kill)
                attempt(lambda: process.wait(timeout=5))
        if self.reader is not None and self.reader.ident is not None:
            attempt(lambda: self.reader.join(timeout=5))
        if self.stderr_reader is not None and self.stderr_reader.ident is not None:
            attempt(lambda: self.stderr_reader.join(timeout=5))
        if process is not None:
            for pipe in (process.stdin, process.stdout, process.stderr):
                if pipe is not None:
                    attempt(pipe.close)
        self.record = {'pid': process.pid if process else None, 'port': self.port,
                       'returncode': process.returncode if process else None,
                       'reaped': process is None or process.returncode is not None,
                       'reader_joined': (self.reader is None or not self.reader.is_alive()) and (self.stderr_reader is None or not self.stderr_reader.is_alive()),
                       'pipes_closed': process is None or (process.stdin.closed and process.stdout.closed and process.stderr.closed),
                       'forced': self.forced, 'cleanup_errors': self.cleanup_errors,
                       'reader_error': self.reader_error, 'original_error': self.original_error,
                       'listen_address': '0.0.0.0', 'client_address': '127.0.0.1', 'argv': self.command}
        final = [line.split()[1:] for line in self.lines if line.startswith('FINAL ')]
        self.record['final'] = dict(zip(FIELDS, map(int, final[0]))) if len(final) == 1 else None
        attempt(lambda: self.log_path.write_text('\n'.join(self.lines)))
        attempt(lambda: self.log_path.with_suffix('.json').write_text(json.dumps(self.record, indent=2)))
        self.cleaned = True
        return self.record

    def __enter__(self):
        return self

    def __exit__(self, kind, failure, traceback):
        if failure:
            self.original_error = repr(failure)
        record = self.cleanup()
        clean = record['reaped'] and record['reader_joined'] and record['pipes_closed'] and not self.cleanup_errors and self.reader_error is None
        if failure:
            if not clean:
                failure.add_note('fixture cleanup: ' + repr(record))
        else:
            require(clean and not self.forced and record['returncode'] == 0, 'normal fixture cleanup: ' + repr(record))
            if self.mode != 'main':
                final = record['final']
                require(final and all(final[key] == 0 for key in ('connections', 'input', 'output', 'jobs')),
                        'final resource leak')
                require(final['admitted'] == final['released'], 'business slot leak')
        return False


def silent_close(client):
    require(not client.pending, 'response before resource close')
    client.socket.settimeout(5)
    try:
        result = client.socket.recv(1)
    except ConnectionResetError:
        result = b''
    require(result == b'', 'resource rejection emitted HTTP/error fallback')


def static_get(port):
    with contextlib.closing(Client(port)) as client:
        client.send(request(headers=b'Connection: close\r\n'))
        client.response(200, b'home', True)
        client.eof()


def check_connections(binary, root, threads, logs):
    with BudgetServer(binary, root, threads, logs, 'connections') as server:
        with contextlib.ExitStack() as clients:
            first = clients.enter_context(contextlib.closing(Client(server.port)))
            second = clients.enter_context(contextlib.closing(Client(server.port)))
            server.wait_state(lambda value: value['connections'] == 2)
            with contextlib.closing(Client(server.port)) as rejected:
                silent_close(rejected)
            state = server.snapshot()
            require(state['reject_connections'] == 1 and state['input'] == 2 * WINDOW, 'joint connection/window accounting')
            first.send(request())
            first.response(200, b'home')
            second.close()
            server.wait_state(lambda value: value['connections'] == 1 and value['input'] == WINDOW)
            static_get(server.port)
        server.wait_state(lambda value: value['connections'] == 0)
    print('CASE Q01 connections full/old survives/recovery', threads, 'PASS', flush=True)


def expect_head():
    return request(b'/api/echo', b'Expect: 100-continue\r\nContent-Length: 1\r\n', b'POST')


def check_input(binary, root, threads, logs):
    with BudgetServer(binary, root, threads, logs, 'input') as server:
        with contextlib.closing(Client(server.port)) as first:
            first.send(expect_head())
            first.continue_response()
            require(server.snapshot()['input'] == WINDOW + REQUEST, '100 retained request reservation')
            with contextlib.closing(Client(server.port)) as rejected:
                rejected.send(expect_head())
                silent_close(rejected)
            state = server.wait_state(lambda value: value['connections'] == 1)
            require(state['reject_input'] == 1 and state['aborted'] == 1 and state['completed'] == 0,
                    'input rejection accounting')
            require(state['parse_errors'] == state['provider_errors'] == 0, 'resource misclassified')
            first.send(b'x' + request())
            first.response(200, b'x')
            first.response(200, b'home')
            server.wait_state(lambda value: value['input'] == WINDOW)
            if os.environ.get('HP_RESOURCE_NEGATIVE'):
                require(False, 'intentional budget recovery wrong expectation')
        server.wait_state(lambda value: value['connections'] == 0)
        with contextlib.closing(Client(server.port)) as fin:
            fin.send(request(b'/api/echo', b'Content-Length: 1\r\n', b'POST') + b'f')
            fin.socket.shutdown(socket.SHUT_WR)
            fin.response(200, b'f')
            fin.eof()
        with contextlib.closing(Client(server.port)) as incomplete:
            incomplete.send(request(b'/api/echo', b'Content-Length: 2\r\n', b'POST') + b'x')
            server.wait_state(lambda value: value['input'] >= REQUEST)
            silent_close(incomplete)  # 500ms绝对读取期限。
        server.wait_state(lambda value: value['input'] == 0)
        static_get(server.port)
    print('CASE Q02 first-input/100/pipeline/FIN/read-timeout/recovery', threads, 'PASS', flush=True)


def check_output(binary, root, threads, logs):
    with BudgetServer(binary, root, threads, logs, 'output') as server:
        with contextlib.closing(Client(server.port)) as slow:
            slow.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 256 * 1024)
            slow.send(request(b'/large.bin', b'Connection: close\r\n'))
            state = server.wait_state(lambda value: value['output'] > 2 * MIB)
            require(state['output'] <= 9 * MIB, 'output bound')
            with contextlib.closing(Client(server.port)) as rejected:
                rejected.send(request(b'/large.bin'))
                silent_close(rejected)
            state = server.snapshot()
            require(state['reject_output'] == 1 and state['provider_errors'] == 0, 'output rejection/fallback')
            slow.response(200, b'\0' * (8 * MIB), True)
            slow.eof()
        server.wait_state(lambda value: value['output'] == 0 and value['connections'] == 0)
        static_get(server.port)
    with BudgetServer(binary, root, threads, logs, 'interim-output') as server:
        with contextlib.closing(Client(server.port)) as rejected:
            rejected.send(expect_head())
            silent_close(rejected)
        with contextlib.closing(Client(server.port)) as rejected:
            rejected.send(request())
            silent_close(rejected)
        state = server.wait_state(lambda value: value['connections'] == 0)
        require(state['reject_output'] == 2 and state['aborted'] == 2 and state['completed'] == 0,
                'interim/final rejection counted as complete or retried')
        require(state['provider_errors'] == 0 and state['parse_errors'] == 0, 'wrong error bucket')
    print('CASE Q03 shared file pending/drain recovery/no recursive500/interim refusal', threads, 'PASS', flush=True)


def check_async(binary, root, threads, logs):
    with BudgetServer(binary, root, threads, logs, 'async') as server:
        with contextlib.ExitStack() as clients:
            running = clients.enter_context(contextlib.closing(TaskClient(server.port)))
            running.send(task_wire(b'POST', body=encoded({'title': 'committed', 'completed': False})))
            server.event('ENTERED')
            second = clients.enter_context(contextlib.closing(TaskClient(server.port)))
            second.send(task_wire())
            server.event('ENTERED')
            queued = clients.enter_context(contextlib.closing(TaskClient(server.port)))
            queued.send(task_wire(b'POST', body=encoded({'title': 'skipped', 'completed': False})))
            state = server.wait_state(lambda value: value['jobs'] == 3)
            require(state['input'] == 3 * (WINDOW + REQUEST), 'awaiting reservation missing')
            for client in (running, second, queued):
                client.task_response(503, {'error': 'task_timeout'}, True)
                client.eof()
        state = server.wait_state(lambda value: value['connections'] == 0 and value['input'] == 0)
        require(state['jobs'] == 2, 'running cancellation freed jobs before return')
        static_get(server.port)
        server.command_line('release')
        server.wait_state(lambda value: value['jobs'] == 0)
        tasks = exchange(server)['tasks']
        require(len(tasks) == 1 and tasks[0]['title'] == 'committed', 'side effects/queued cancellation')
    with BudgetServer(binary, root, threads, logs, 'async') as server:
        with contextlib.closing(TaskClient(server.port)) as pending:
            pending.send(task_wire())
            server.event('ENTERED')
            server.process.send_signal(signal.SIGTERM)
            server.event('NETWORK_STOPPED')
            silent_close(pending)
            state = server.snapshot()
            require(state['connections'] == state['input'] == state['output'] == 0 and state['jobs'] == 1,
                    'network and worker ownership conflated')
            server.process.send_signal(signal.SIGTERM)
            server.command_line('release')
            server.wait_state(lambda value: value['jobs'] == 0)
    print('CASE Q04 business awaiting/timeout/late result/drain independent slots', threads, 'PASS', flush=True)


def read_metrics(server):
    # Readers have joined before consumption; only stdout owns the metrics protocol.
    text = '\n'.join(server.stdout_lines).split('HP_METRICS_BEGIN\n', 1)[1].split('HP_METRICS_END', 1)[0]
    return {key: int(value) for key, value in (line.split() for line in text.splitlines())}


class ScheduledPipe:
 def __init__(self,pipe,protocol,release,appended):
  self.pipe=pipe;self.protocol=protocol;self.release=release;self.appended=appended;self.after_marker=False;self.first=True
 def readline(self,limit):
  if self.protocol and self.after_marker:
   self.release.set()
   if not self.appended.wait(5):raise RuntimeError('stderr schedule deadline')
   self.after_marker=False
  if not self.protocol:
   if self.first:
    self.first=False
    if not self.release.wait(5):raise RuntimeError('metrics schedule deadline')
   else:self.appended.set()
  data=self.pipe.readline(limit)
  if self.protocol and data.strip()==b'HP_METRICS_BEGIN':self.after_marker=True
  return data
class ScheduledServer(BudgetServer):
 def __init__(self,*args):
  self.release_stderr=threading.Event();self.stderr_appended=threading.Event();super().__init__(*args)
 def collect_stream(self,stream,protocol):
  super().collect_stream(ScheduledPipe(stream,protocol,self.release_stderr,self.stderr_appended),protocol)


def check_metrics_reader_order(binary, root, logs):
    with ScheduledServer(binary, root, 0, logs, 'main') as server:
        static_get(server.port)
    mixed = server.log_path.read_text().split('HP_METRICS_BEGIN\n', 1)[1].split('HP_METRICS_END', 1)[0]
    require(any(line.startswith('STDERR ') for line in mixed.splitlines()),
            'real stderr must be appended after stdout BEGIN before metrics continue')
    metrics = read_metrics(server)
    require(len([key for key in metrics if key.startswith('resource_')]) == 9, 'scheduled resource count')
    for key in ('connections', 'input_bytes', 'output_bytes'):
        require(metrics[f'resource_{key}_current'] == 0, 'scheduled current leak')
        require(metrics[f'resource_{key}_peak'] > 0, 'scheduled peak missing')
        require(metrics[f'resource_{key}_rejected_total'] == 0, 'scheduled unexpected rejection')
    (logs / 'metrics-reader-order.json').write_text(json.dumps({
        'stderr_inside_metrics': True, 'consumer_error': None, 'metrics': metrics,
        'cleanup': server.record}, indent=2))
    print('CASE Q07 real stdout BEGIN/stderr/stdout metrics source isolation PASS', flush=True)


def check_default_main(binary, root, logs):
    with BudgetServer(binary, root, 2, logs, 'main') as server:
        with contextlib.ExitStack() as clients:
            idle = [clients.enter_context(contextlib.closing(Client(server.port))) for _ in range(128)]
            with contextlib.closing(Client(server.port)) as rejected:
                silent_close(rejected)
            for client in idle[:61]:
                client.send(expect_head())
                client.continue_response()
            idle[61].send(expect_head())
            silent_close(idle[61])
            for client in idle[:61]:
                client.send(b'x')
                client.response(200, b'x')
            idle[0].send(request())
            idle[0].response(200, b'home')
    metrics = read_metrics(server)
    require(len([key for key in metrics if key.startswith('resource_')]) == 9, 'resource field count')
    require(all(metrics[f'resource_{key}_current'] == 0 for key in ('connections', 'input_bytes', 'output_bytes')), 'main final leak')
    require(metrics['resource_connections_peak'] == 128 and metrics['resource_input_bytes_peak'] <= 64 * MIB,
            'default main bounds')
    require(metrics['resource_connections_rejected_total'] == 1 and metrics['resource_input_bytes_rejected_total'] == 1,
            'default main rejects')
    print('CASE Q05 production defaults 128 connections/61 requests/9 exit fields PASS', flush=True)


def check_failure_cleanup(binary, root, logs):
    records = []
    for mode in ('before-port', 'after-port', 'during-run'):
        marker = RuntimeError('injected resource fixture ' + mode)
        server = None
        failure = None
        rescue = False
        class StartupFailure(BudgetServer):
            def wait_port(self):
                if mode == 'after-port':
                    self.port = super().wait_port()
                raise marker
        try:
            if mode == 'during-run':
                server = BudgetServer(binary, root, 0, logs, 'input')
                with server:
                    static_get(server.port)
                    raise marker
            else:
                server = StartupFailure.__new__(StartupFailure)
                StartupFailure.__init__(server, binary, root, 0, logs, 'input')
        except BaseException as caught:
            failure = caught
        finally:
            if server is not None and (server.process.poll() is None or server.reader.is_alive() or (server.stderr_reader and server.stderr_reader.is_alive())):
                rescue = True
                server.cleanup()
        require(failure is marker and not rescue, 'original failure/own cleanup')
        record = server.record
        require(record['reaped'] and record['reader_joined'] and record['pipes_closed'], 'resource fixture leak')
        records.append({'mode': mode, 'outer_rescue': rescue, **record})
    (logs / 'failure-cleanup.json').write_text(json.dumps(records, indent=2))
    print('CASE Q06 real PORT before/after and running failure own cleanup PASS', flush=True)


def main():
    binary, fixture_binary = sys.argv[1:]
    base = Path(os.environ['HP_S3_TEST_TMP_ROOT'])
    base.mkdir(parents=True, exist_ok=True)
    logs = Path(tempfile.mkdtemp(prefix='resource-evidence-', dir=base))
    root = None
    passed = False
    original = None
    signal.signal(signal.SIGTERM, interrupt_suite)
    signal.signal(signal.SIGALRM, interrupt_suite)
    signal.alarm(120)
    try:
        root = Path(tempfile.mkdtemp(prefix='resource-static-', dir=base))
        (root / 'index.html').write_bytes(b'home')
        with (root / 'large.bin').open('wb') as output:
            output.truncate(8 * MIB)
        check_metrics_reader_order(binary, root, logs)
        check_failure_cleanup(fixture_binary, root, logs)
        for threads in (0, 2):
            check_connections(fixture_binary, root, threads, logs)
            check_input(fixture_binary, root, threads, logs)
            check_output(fixture_binary, root, threads, logs)
            check_async(fixture_binary, root, threads, logs)
        check_default_main(binary, root, logs)
        passed = True
    except BaseException as failure:
        original = failure
        raise
    finally:
        cleanup_errors = []
        if root is not None:
            try:
                shutil.rmtree(root)
            except BaseException as failure:
                cleanup_errors.append(repr(failure))
        summary = {'passed': passed and not cleanup_errors, 'root_removed': root is None or not root.exists(),
                   'original_error': repr(original) if original else None, 'cleanup_errors': cleanup_errors,
                   'listen_address': '0.0.0.0', 'client_address': '127.0.0.1'}
        try:
            (logs / 'summary.json').write_text(json.dumps(summary, indent=2))
        except BaseException as failure:
            cleanup_errors.append(repr(failure))
        signal.alarm(0)
        print('RESOURCE_SUMMARY', logs / 'summary.json', flush=True)
        if cleanup_errors:
            if original:
                original.add_note('cleanup: ' + repr(cleanup_errors))
            else:
                raise RuntimeError(repr(cleanup_errors))


if __name__ == '__main__':
    main()
