#!/usr/bin/env python3
"""固定长度正文、读取绝对期限和观测的真实HTTP测试。"""
import contextlib
import json
import os
from pathlib import Path
import queue
import select
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

from HttpProtocolBaselineHttp_test import Client, Server, require, request, remaining, interrupt_suite

BODY_LIMIT = 1024 * 1024
ERRORS = {400: b"400 Bad Request\n", 405: b"405 Method Not Allowed\n", 413: b"413 Content Too Large\n"}


class BodyServer(Server):
    def __init__(self, binary, root, threads, logs, read_timeout=300, idle=1500, keep_alive=1500):
        self.port_queue = queue.Queue()
        self.log_error = None
        self.log_path = logs / f"server-{threads}-{read_timeout}-{time.monotonic_ns()}.log"
        self.command = [binary, "--port", "0", "--root", str(root), "--threads", str(threads),
                        "--idle-timeout-ms", str(idle), "--keep-alive-timeout-ms", str(keep_alive),
                        "--metrics-on-exit", "--access-log"]
        if read_timeout is not None:
            self.command += ["--request-read-timeout-ms", str(read_timeout)]
        self.process = subprocess.Popen(self.command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.reader = threading.Thread(target=self.collect_output, daemon=True)
        self.reader.start()
        self.stopped = False


@contextlib.contextmanager
def running_server(binary, root, threads, logs, **options):
    server = BodyServer(binary, root, threads, logs, **options)
    failed = True
    try:
        yield server.start()
        failed = False
    finally:
        server.stop(failed)


def body_request(body, method=b"GET", close=False):
    headers = b"Content-Length: " + str(len(body)).encode() + b"\r\n"
    if close:
        headers += b"Connection: close\r\n"
    return request(headers=headers, method=method) + body


def check_body_pipeline(server):
    for body in [b"x", b"a\0b\r\n\xff", request(), bytes(range(256)) * (BODY_LIMIT // 256)]:
        print("CASE H01-body", len(body), flush=True)
        with contextlib.closing(Client(server.port)) as client:
            client.send(body_request(body) + request(headers=b"Connection: close\r\n"))
            client.response(200, b"home\n")
            client.response(200, b"home\n", True)
            client.eof()
    for method in [b"POST", b"HEAD"]:
        with contextlib.closing(Client(server.port)) as client:
            client.send(body_request(b"abc", method=method))
            client.response(405, ERRORS[405], True)
            client.eof()
    print("CASE H01-methods-no-business PASS", flush=True)


def check_rejections(server):
    cases = [(b"Content-Length: 1048577\r\n", 413),
             (b"Content-Length: 18446744073709551615\r\n", 413),
             (b"Content-Length: 18446744073709551616\r\n", 400),
             (b"Content-Length: 1048577\r\nTransfer-Encoding: chunked\r\n", 400),
             (b"Content-Length: 1048577\r\nExpect: 100-continue\r\n", 400),
             (b"Content-Length: 1048577\r\nContent-Length: 0\r\n", 400),
             (b"Content-Length: +1\r\n", 400)]
    for headers, status in cases:
        print("CASE H02-reject", status, headers.split(b"\r\n")[0].decode(), flush=True)
        with contextlib.closing(Client(server.port)) as client:
            # 超限只发送头部，证明不会等待或预读声明正文。
            client.send(request(headers=headers))
            client.response(status, ERRORS[status], True)
            client.eof()
    with contextlib.closing(Client(server.port)) as client:
        client.send(request(headers=b"Content-Length: 3\r\n") + b"x")
        client.socket.shutdown(socket.SHUT_WR)
        client.response(400, ERRORS[400], True)
        client.eof()
    print("CASE H02-truncated-body-fin PASS", flush=True)


def wait_readable(client, duration):
    deadline = min(client.deadline, time.monotonic() + duration)
    readable, _, _ = select.select([client.socket], [], [], remaining(deadline))
    return bool(readable)


def check_drip(server, prefix, case_id):
    with contextlib.closing(Client(server.port)) as client:
        started = time.monotonic()
        client.send(prefix)
        sent = 0
        closed = False
        while time.monotonic() - started < 0.8:
            if wait_readable(client, 0.04):
                client.socket.settimeout(remaining(client.deadline))
                require(client.socket.recv(1) == b"", "deadline produced unexpected response")
                closed = True
                break
            client.send(b"x")
            sent += 1
        elapsed = time.monotonic() - started
        require(closed and 0.18 <= elapsed < 0.8 and sent >= 3, "absolute deadline missing or premature")
        print("CASE", case_id, "seconds", round(elapsed, 4), "progress_writes", sent, flush=True)
    with contextlib.closing(Client(server.port)) as other:
        other.send(request(headers=b"Connection: close\r\n"))
        other.response(200, b"home\n", True)
        other.eof()


def check_fresh_deadline(server):
    with contextlib.closing(Client(server.port)) as client:
        client.send(body_request(b"first"))
        client.response(200, b"home\n")
        # 没有未完成请求时超过原300ms期限仍保活。
        require(not wait_readable(client, 0.4), "old deadline closed idle connection")
        client.send(request(headers=b"Content-Length: 3\r\n") + b"a")
        require(not wait_readable(client, 0.08), "second deadline inherited first")
        client.send(b"bc")
        client.response(200, b"home\n")
        client.send(request(headers=b"Connection: close\r\n"))
        client.response(200, b"home\n", True)
        client.eof()
    print("CASE H04-fresh-deadline PASS", flush=True)


def read_metrics(server):
    text = server.log_path.read_text()
    metrics = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[1].isdigit():
            metrics[fields[0]] = int(fields[1])
    require(metrics["connections_active"] == 0 and metrics["logger_pending"] == 0, "resources not drained")
    require(metrics["requests_started_total"] == metrics["responses_completed_total"] + metrics["requests_aborted_total"], "request accounting mismatch")
    return metrics, text


def check_observability(binary, root, threads, logs):
    with running_server(binary, root, threads, logs) as server:
        with contextlib.closing(Client(server.port)) as client:
            client.send(request(headers=b"Content-Length: 1048577\r\n"))
            client.response(413, ERRORS[413], True)
            client.eof()
        check_drip(server, request(headers=b"Content-Length: 1000\r\n") + b"secret-body-not-in-access-log", "H05-abort-once")
    metrics, text = read_metrics(server)
    require(metrics["responses_status_413_total"] == 1 and metrics["parse_errors_total"] == 1, "413 accounting")
    require(metrics["requests_aborted_total"] == 1 and metrics["responses_status_unknown_total"] == 1, "timeout accounting")
    require(metrics["responses_completed_total"] == 2 and metrics["errors_total"] == 2, "completion/error accounting")
    require("secret-body-not-in-access-log" not in text and '"status":413' in text, "body leaked or access 413 missing")
    print("CASE H05-observability PASS", flush=True)


def check_disabled_and_shutdown(binary, root, threads, logs):
    with running_server(binary, root, threads, logs, read_timeout=0) as server:
        with contextlib.closing(Client(server.port)) as client:
            client.send(request(headers=b"Content-Length: 3\r\n") + b"a")
            require(not wait_readable(client, 0.4), "zero did not disable read deadline")
            client.send(b"bc")
            client.response(200, b"home\n")
        with contextlib.closing(Client(server.port)) as client:
            client.send(request(headers=b"Content-Length: 9\r\n") + b"x")
            client.socket.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        with contextlib.closing(Client(server.port)) as client:
            client.send(request(headers=b"Connection: close\r\n"))
            client.response(200, b"home\n", True)
            client.eof()
        with contextlib.closing(Client(server.port)) as client:
            client.send(request(headers=b"Content-Length: 100\r\n") + b"x")
            # 先通过另一连接完整响应确认owner仍在处理，而非以sleep当收到请求的证据。
            with contextlib.closing(Client(server.port)) as witness:
                witness.send(request(headers=b"Connection: close\r\n"))
                witness.response(200, b"home\n", True)
                witness.eof()
            server.stop()
    read_metrics(server)
    print("CASE H06-disabled-rst-shutdown PASS", flush=True)


def check_cli(binary, root, logs):
    result = subprocess.run([binary, "--help"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=5)
    require(result.returncode == 0 and b"--request-read-timeout-ms <0-86400000> defaults to 10000" in result.stdout, "help/default mismatch")
    (logs / "help.log").write_bytes(result.stdout)
    records = []
    for extra in [["-1"], ["86400001"], ["x"], [""], [], ["1", "--request-read-timeout-ms", "2"]]:
        command = [binary, "--port", "0", "--root", str(root), "--request-read-timeout-ms"] + extra
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=5)
        require(len(result.stdout) <= 16 * 1024 * 1024 and result.returncode == 2, "invalid CLI accepted")
        records.append({"argv": command, "exit": result.returncode})
    (logs / "cli.json").write_text(json.dumps(records, indent=2))
    for duration in [None, 86400000]:
        with running_server(binary, root, 0, logs, read_timeout=duration) as server:
            with contextlib.closing(Client(server.port)) as client:
                client.send(body_request(b"configured", close=True))
                client.response(200, b"home\n", True)
                client.eof()
    print("CASE H07-cli PASS", flush=True)


def main():
    binary = str(Path(sys.argv[1]).resolve())
    parent = Path(os.environ.get("HP_S3_TEST_TMP_ROOT", tempfile.gettempdir()))
    parent.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="http-body-", dir=parent))
    root, logs = run / "www", run / "logs"
    root.mkdir()
    logs.mkdir()
    (root / "index.html").write_bytes(b"home\n")
    signal.signal(signal.SIGALRM, interrupt_suite)
    signal.alarm(120)
    passed = False
    try:
        check_cli(binary, root, logs)
        for threads in [0, 2]:
            print("WORKERS", threads, flush=True)
            with running_server(binary, root, threads, logs, read_timeout=2000) as server:
                check_body_pipeline(server)
                check_rejections(server)
            with running_server(binary, root, threads, logs) as server:
                check_drip(server, b"GET / HTTP/1.1\r\nHost: ", "H03-header-drip")
                check_drip(server, request(headers=b"Content-Length: 1000\r\n"), "H03-body-drip")
                check_fresh_deadline(server)
            check_observability(binary, root, threads, logs)
            check_disabled_and_shutdown(binary, root, threads, logs)
        passed = True
    finally:
        signal.alarm(0)
        shutil.rmtree(root)
        (logs / "summary.json").write_text(json.dumps({"passed": passed, "fixtures_removed": not root.exists()}))
        print("EVIDENCE", logs, "fixtures_removed", not root.exists(), flush=True)
    print("Request body HTTP baseline PASS", flush=True)


if __name__ == "__main__":
    main()
