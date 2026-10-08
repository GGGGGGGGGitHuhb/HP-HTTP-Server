#!/usr/bin/env python3
"""V0.6/S1 independent fixtures: CLI, HTTP, final accounting and JSON consumers."""
import json
import os
import pathlib
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

BINARY = str(pathlib.Path(sys.argv[1]).resolve())
TMP = pathlib.Path(os.environ['HP_S3_TEST_TMP_ROOT'])
TMP.mkdir(parents=True, exist_ok=True)
RUN = pathlib.Path(tempfile.mkdtemp(prefix='s1-observability-', dir=TMP))
ROOT = RUN / 'root'
ROOT.mkdir()
(ROOT / 'index.html').write_bytes(b'hello-observability')
(ROOT / 'empty').write_bytes(b'')
(ROOT / 'large').write_bytes(b'L' * (8 * 1024 * 1024))
(ROOT / 'escape').symlink_to('/etc/passwd')


class Server:
    def __init__(self, name, workers=0, options=(), full=False):
        self.stdout_path = RUN / (name + '.stdout')
        self.stderr_path = RUN / (name + '.stderr')
        self.stdout = open('/dev/full' if full else self.stdout_path, 'wb')
        self.stderr = self.stderr_path.open('wb')
        self.command = [BINARY, '--port', '0', '--root', str(ROOT), '--threads', str(workers),
                        '--idle-timeout-ms', '1000', '--keep-alive-timeout-ms', '1000', *options]
        self.process = subprocess.Popen(self.command, stdout=self.stdout, stderr=self.stderr)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            text = self.stderr_path.read_text()
            marker = 'Listening on TCP port '
            if marker in text:
                self.port = int(text.split(marker)[1].split('.')[0])
                break
            if self.process.poll() is not None:
                raise AssertionError(('premature exit', self.command, self.process.returncode, text))
            time.sleep(0.01)
        else:
            self.stop()
            raise AssertionError('server readiness timeout')

    def connect(self):
        peer = socket.create_connection(('127.0.0.1', self.port), timeout=5)
        peer.settimeout(5)
        return peer

    def stop(self):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
        try:
            result = self.process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            raise AssertionError('server required SIGKILL')
        finally:
            self.stdout.close()
            self.stderr.close()
        return result

    def metrics(self):
        output = self.stdout_path.read_text()
        assert output.count('HP_METRICS_BEGIN') == output.count('HP_METRICS_END') == 1, output
        body = output.split('HP_METRICS_BEGIN\n')[1].split('HP_METRICS_END')[0]
        values = {}
        for line in body.splitlines():
            key, value = line.split()
            assert key not in values
            values[key] = int(value)
        assert values['requests_started_total'] == values['responses_completed_total'] + values['requests_aborted_total']
        assert values['latency_count'] == values['requests_started_total']
        assert values['connections_active'] == values['logger_pending'] == values['logger_truncated'] == 0
        assert sum(v for k, v in values.items() if k.startswith('responses_status_')) == values['requests_started_total']
        return values

    def records(self):
        result = []
        for line in self.stderr_path.read_text().splitlines():
            if '"event":"http_access"' in line:
                assert line.startswith('[INFO] ')
                payload = line[len('[INFO] '):]
                assert len(payload.encode()) <= 1024
                record = json.loads(payload)
                assert set(record) == {'event', 'method', 'path', 'status', 'content_bytes', 'duration_us', 'outcome', 'path_truncated'}
                result.append(record)
        return result


class ResponseReader:
    def __init__(self, peer):
        self.peer = peer
        self.buffer = b''

    def response(self):
        while b'\r\n\r\n' not in self.buffer:
            part = self.peer.recv(65536)
            assert part, 'early header EOF'
            self.buffer += part
        header, self.buffer = self.buffer.split(b'\r\n\r\n', 1)
        status = int(header.split(b' ')[1])
        length = int(dict(line.split(b':', 1) for line in header.split(b'\r\n')[1:])[b'Content-Length'])
        while len(self.buffer) < length:
            part = self.peer.recv(65536)
            assert part, 'early body EOF'
            self.buffer += part
        body, self.buffer = self.buffer[:length], self.buffer[length:]
        return status, body


def get(path=b'/', close=True):
    return b'GET ' + path + b' HTTP/1.1\r\nHost: test\r\n' + (b'Connection: close\r\n' if close else b'') + b'\r\n'


def exchange(server, request, status, body=None):
    with server.connect() as peer:
        peer.sendall(request)
        actual, content = ResponseReader(peer).response()
        assert actual == status, (actual, status)
        if body is not None:
            assert content == body
        return content


def test_cli():
    encoded = subprocess.run([str(pathlib.Path(BINARY).with_name("http_observability_tests")), "--json"], capture_output=True, check=True)
    encoded_lines = encoded.stdout.splitlines()
    assert len(encoded_lines) == 2
    record = json.loads(encoded_lines[0])
    assert record["method"] == "".join(chr(byte) for byte in range(16))
    assert record["path"] == "".join(chr(byte) for byte in range(35))
    assert not record["path_truncated"] and len(encoded_lines[0]) <= 1024
    worst = json.loads(encoded_lines[1])
    assert worst["method"] == "\x80" * 16 and worst["path"] == "\x80" * 96
    assert worst["path_truncated"] and worst["duration_us"] == 2**64 - 1
    assert len(encoded_lines[1]) <= 1024
    help_result = subprocess.run([BINARY, '--help'], capture_output=True)
    assert help_result.returncode == 0
    assert b'--access-log' in help_result.stdout and b'--metrics-on-exit' in help_result.stdout
    base = [BINARY, '--port', '0', '--root', str(ROOT)]
    for extra in (['--access-log', '--access-log'], ['--metrics-on-exit', '--metrics-on-exit'],
                  ['--access-log', 'true'], ['--metrics-on-exit', '1'], ['--unknown']):
        result = subprocess.run(base + extra, capture_output=True)
        assert result.returncode == 2 and b'HP_METRICS_BEGIN' not in result.stdout
    default = Server('default')
    try:
        exchange(default, get(), 200, b'hello-observability')
    finally:
        assert default.stop() == 0
    assert 'HP_METRICS_' not in default.stdout_path.read_text()
    assert not default.records()
    failed = Server('stdout-failed', options=['--metrics-on-exit'], full=True)
    assert failed.stop() == 1


def test_http(workers):
    server = Server(f'http-{workers}', workers, ['--access-log', '--metrics-on-exit'])
    expected = []
    try:
        # 无数据连接，EOF与RST都不应制造HTTP请求。
        with server.connect():
            pass
        with server.connect() as peer:
            peer.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        exchange(server, get(), 200, b'hello-observability'); expected.append(200)
        exchange(server, get(b'/empty'), 200, b''); expected.append(200)
        exchange(server, get(b'/missing?QUERY_SECRET'), 404); expected.append(404)
        exchange(server, get(b'/escape'), 403); expected.append(403)
        exchange(server, b'POST / HTTP/1.1\r\nHost: test\r\n\r\n', 405); expected.append(405)
        exchange(server, b'BAD_REQUEST\r\n\r\n', 400); expected.append(400)
        exchange(server, get(b'/' + b'a' * 2000 + b'?QUERY_SECRET'), 500); expected.append(500)
        exchange(server, get(b'/quote"back\\slash'), 403); expected.append(403)
        exchange(server, get(b'/nonascii\xc3\xa9'), 404); expected.append(404)
        with server.connect() as peer:
            peer.sendall(get(b'/', False) + get(b'/empty'))
            reader = ResponseReader(peer)
            assert reader.response() == (200, b'hello-observability')
            assert reader.response() == (200, b'')
        expected += [200, 200]
        exchange(server, get(b'/large'), 200, b'L' * (8 * 1024 * 1024)); expected.append(200)
        with server.connect() as peer:
            peer.sendall(b'GET / HTTP/1.1\r\nHost: test\r\n')
            peer.shutdown(socket.SHUT_WR)
            assert ResponseReader(peer).response()[0] == 400
        expected.append(400)
        with server.connect() as peer:
            peer.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            peer.sendall(get(b'/large', False))
            assert peer.recv(1024).startswith(b'HTTP/1.1 200')
            peer.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        # 另一个请求确认RST处理后仍可服务。
        exchange(server, get(), 200, b'hello-observability'); expected.append(200)
    finally:
        assert server.stop() == 0
    metrics = server.metrics()
    records = server.records()
    assert metrics['requests_started_total'] == len(expected) + 1
    assert metrics['requests_aborted_total'] == 1
    assert metrics['responses_completed_total'] == len(expected)
    assert len(records) == metrics['requests_started_total']
    assert sum(record['outcome'] == 'aborted' for record in records) == 1
    assert metrics['errors_total'] == sum(status >= 400 for status in expected) + 1
    for status in (200, 400, 403, 404, 405, 500):
        assert metrics[f'responses_status_{status}_total'] == expected.count(status) + (status == 200)
    logtext = server.stderr_path.read_text()
    assert 'QUERY_SECRET' not in logtext and 'FRAGMENT_SECRET' not in logtext
    assert any(record['path_truncated'] for record in records)
    assert any(record['path'] == '/quote"back\\slash' for record in records)
    assert metrics['access_log_failures_total'] == 0 and metrics['logger_failed'] == 0
    assert metrics['connections_total'] >= len(expected) + 2
    return {'workers': workers, 'metrics': metrics, 'records': len(records)}


def test_graceful_completed(workers):
    server = Server(f'graceful-completed-{workers}', workers,
                    ['--access-log', '--metrics-on-exit', '--shutdown-timeout-ms', '5000'])
    peer = server.connect()
    try:
        # Pipeline后缀已在输入中，但drain不能开始该请求或调用provider。
        peer.sendall(get(b'/large', False) + get(b'/'))
        first = peer.recv(1024)
        assert first.startswith(b'HTTP/1.1 200')
        deadline = time.monotonic() + 3
        pending_bytes = 0
        while time.monotonic() < deadline:
            text = server.stderr_path.read_text()
            marker = 'S3 evidence: connection write reached EAGAIN with '
            if marker in text:
                pending_bytes = int(text.split(marker)[-1].split(' ')[0])
                if pending_bytes > 0:
                    break
            time.sleep(0.01)
        assert pending_bytes > 0, 'must prove output pending before shutdown'
        server.process.send_signal(signal.SIGTERM)
        time.sleep(0.1)
        reader = ResponseReader(peer)
        reader.buffer = first
        assert reader.response() == (200, b'L' * (8 * 1024 * 1024))
        assert not reader.buffer and peer.recv(1) == b'', 'drain must not serve pipeline suffix'
        assert server.stop() == 0
    finally:
        peer.close()
        if server.process.poll() is None:
            server.stop()
    metrics = server.metrics()
    records = server.records()
    assert metrics['requests_started_total'] == metrics['responses_completed_total'] == 1
    assert metrics['requests_aborted_total'] == metrics['errors_total'] == 0
    assert len(records) == 1 and records[0]['outcome'] == 'completed'
    assert records[0]['content_bytes'] == 8 * 1024 * 1024 and records[0]['path'] == '/large'
    return {'workers': workers, 'pending_before_signal': pending_bytes,
            'body_bytes': 8 * 1024 * 1024, 'pipeline_suffix_served': False, 'metrics': metrics}


def test_forced(workers, timeout):
    server = Server(f'forced-{workers}-{timeout}', workers,
                    ['--access-log', '--metrics-on-exit', '--shutdown-timeout-ms', str(timeout)])
    peer = server.connect()
    try:
        peer.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        peer.sendall(get(b'/large', False))
        assert peer.recv(1024).startswith(b'HTTP/1.1 200')
        assert server.stop() == 0
    finally:
        peer.close()
        if server.process.poll() is None:
            server.stop()
    metrics = server.metrics()
    assert metrics['requests_started_total'] == metrics['requests_aborted_total'] == 1
    assert metrics['responses_completed_total'] == 0
    records = server.records()
    assert len(records) == 1 and records[0]['outcome'] == 'aborted' and records[0]['content_bytes'] == 8 * 1024 * 1024
    return {'workers': workers, 'timeout_ms': timeout, 'metrics': metrics}


def main():
    results = []
    try:
        test_cli()
        for workers in (0, 2):
            results.append(test_http(workers))
            results.append(test_graceful_completed(workers))
            for timeout in (0, 50):
                results.append(test_forced(workers, timeout))
        (RUN / 'result.json').write_text(json.dumps({'status': 'passed', 'cases': results}, indent=2))
        print('CLI/default/output failure; 0/2 workers HTTP/errors/pipeline/sendfile/empty/FIN/RST/JSON/graceful/force: passed')
        print('raw fixtures:', RUN)
    except BaseException as error:
        (RUN / 'result.json').write_text(json.dumps({'status': 'failed', 'error': repr(error), 'cases': results}, indent=2))
        raise


if __name__ == '__main__':
    main()
