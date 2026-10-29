#!/usr/bin/env python3
"""当前HTTP子集的真实进程回归；仅使用标准库，无性能测量。"""
import contextlib
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

MAX_RESPONSE = 9 * 1024 * 1024
MAX_LOG = 16 * 1024 * 1024
ERRORS = {400: b"400 Bad Request\n", 403: b"403 Forbidden\n",
          404: b"404 Not Found\n", 405: b"405 Method Not Allowed\n"}


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def remaining(deadline):
    value = deadline - time.monotonic()
    if value <= 0:
        raise TimeoutError("absolute scenario deadline")
    return value


def request(path=b"/", headers=b"", method=b"GET"):
    return method + b" " + path + b" HTTP/1.1\r\nHost: baseline\r\n" + headers + b"\r\n"


class Client:
    def __init__(self, port):
        self.deadline = time.monotonic() + 5
        self.socket = socket.create_connection(("127.0.0.1", port), remaining(self.deadline))
        self.pending = b""

    def close(self):
        self.socket.close()

    def send(self, data):
        self.socket.settimeout(remaining(self.deadline))
        self.socket.sendall(data)

    def receive(self):
        self.socket.settimeout(remaining(self.deadline))
        block = self.socket.recv(65536)
        require(block != b"", "unexpected EOF within response")
        self.pending += block
        require(len(self.pending) <= MAX_RESPONSE, "response exceeds limit")

    def response(self, status, expected_body, close=False, method=b"GET", allow=b"GET, HEAD"):
        while b"\r\n\r\n" not in self.pending:
            self.receive()
            require(len(self.pending) <= MAX_RESPONSE, "missing header boundary")
        raw_header, self.pending = self.pending.split(b"\r\n\r\n", 1)
        lines = raw_header.split(b"\r\n")
        require(lines[0].startswith(b"HTTP/1.1 " + str(status).encode() + b" "), "wrong status")
        headers = {}
        for line in lines[1:]:
            name, value = line.split(b":", 1)
            name = name.lower()
            require(name not in headers, "duplicate response header")
            headers[name] = value.strip()
        require(headers[b"content-length"].isdigit(), "invalid Content-Length")
        length = int(headers[b"content-length"])
        require(length == len(expected_body) and length <= MAX_RESPONSE, "wrong body length")
        require(headers[b"connection"] == (b"close" if close else b"keep-alive"), "wrong connection policy")
        if status == 405:
            require(headers.get(b"allow") == allow, "wrong Allow")
        else:
            require(b"allow" not in headers, "unexpected Allow")
        if method == b"HEAD":
            length = 0
            expected_body = b""
        while len(self.pending) < length:
            self.receive()
        body, self.pending = self.pending[:length], self.pending[length:]
        require(hashlib.sha256(body).digest() == hashlib.sha256(expected_body).digest(), "body mismatch")
        return headers

    def continue_response(self):
        while b"\r\n\r\n" not in self.pending:
            self.receive()
        interim, self.pending = self.pending.split(b"\r\n\r\n", 1)
        require(interim == b"HTTP/1.1 100 Continue", "invalid or duplicate interim response")

    def eof(self):
        require(not self.pending, "extra response suffix")
        self.socket.settimeout(remaining(self.deadline))
        require(self.socket.recv(1) == b"", "expected orderly EOF without suffix")


class Server:
    def __init__(self, binary, root, threads, logs, idle=5000, keep_alive=5000):
        self.port_queue = queue.Queue()
        self.log_error = None
        self.log_path = logs / f"server-{threads}-{idle}-{keep_alive}-{time.monotonic_ns()}.log"
        self.command = [binary, "--port", "0", "--root", str(root), "--threads", str(threads),
                        "--idle-timeout-ms", str(idle), "--keep-alive-timeout-ms", str(keep_alive)]
        self.process = subprocess.Popen(self.command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.reader = threading.Thread(target=self.collect_output, daemon=True)
        self.reader.start()
        self.stopped = False

    def collect_output(self):
        size = 0
        pending = b""
        try:
            with self.log_path.open("wb") as output:
                while True:
                    block = os.read(self.process.stdout.fileno(), 4096)
                    if not block:
                        break
                    if size + len(block) > MAX_LOG:
                        self.log_error = "server log exceeded 16MiB"
                        self.process.kill()
                        break
                    output.write(block)
                    size += len(block)
                    pending = (pending + block)[-8192:]
                    match = re.search(rb"listening on port ([0-9]+)\.", pending)
                    if match and self.port_queue.empty():
                        self.port_queue.put(int(match.group(1)))
        except Exception as error:
            self.log_error = repr(error)

    def start(self):
        self.port = self.port_queue.get(timeout=5)
        return self

    def stop(self, failed=False):
        if self.stopped:
            return
        forced = False
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
        try:
            code = self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            forced = True
            self.process.kill()
            code = self.process.wait(timeout=5)
        self.reader.join(timeout=5)
        self.process.stdout.close()
        self.stopped = True
        record = {"pid": self.process.pid, "returncode": code, "forced": forced,
                  "reaped": self.process.poll() is not None, "reader_joined": not self.reader.is_alive(),
                  "failed_scenario": failed, "log_error": self.log_error, "argv": self.command}
        self.log_path.with_suffix(".json").write_text(json.dumps(record, indent=2))
        require(not forced and code == 0 and not self.reader.is_alive() and self.log_error is None,
                f"server cleanup failed: {record}")


@contextlib.contextmanager
def running_server(binary, root, threads, logs, **options):
    server = Server(binary, root, threads, logs, **options)
    failed = True
    try:
        yield server.start()
        failed = False
    finally:
        server.stop(failed)


def check_exchange(server, case_id, wire, status, body, close=True):
    print("CASE", case_id, flush=True)
    with contextlib.closing(Client(server.port)) as client:
        client.send(wire)
        client.response(status, body, close)
        if close:
            client.eof()


def check_static(server, binary_body, large_body):
    for path, body in [(b"/", b"home\n"), (b"/small.txt", b"small\n"),
                       (b"/binary.dat", binary_body), (b"/limit.dat", large_body),
                       (b"/small.txt?q=x", b"small\n")]:
        check_exchange(server, "P05-" + path.decode(), request(path, b"Connection: close\r\n"), 200, body)
    for path, status in [(b"/missing", 404), (b"/../outside", 403), (b"/a\\b", 403),
                         (b"/link", 403), (b"/dirlink/file", 403), (b"/%2e", 400),
                         (b"/small.txt?q=%20", 400), (b"/over.dat", 403)]:
        check_exchange(server, "P05-" + path.decode(), request(path, b"Connection: close\r\n"), status, ERRORS[status])


def check_protocol(server):
    print("CASE P04-pipeline-200-404-200", flush=True)
    with contextlib.closing(Client(server.port)) as client:
        client.send(request() + request(b"/missing") + request(b"/small.txt", b"Connection: close\r\n"))
        client.response(200, b"home\n")
        client.response(404, ERRORS[404])
        client.response(200, b"small\n", True)
        client.eof()
    print("CASE P03-segmented-and-P04-sequential", flush=True)
    with contextlib.closing(Client(server.port)) as client:
        for block in [b"GET / HTTP/1.1\r", b"\nHost: baseline\r\nContent-Len", b"gth: 000\r\n\r", b"\n"]:
            client.send(block)
        client.response(200, b"home\n")
        client.send(request(b"/small.txt", b"Connection: keep-alive, CLOSE\r\n") + request())
        client.response(200, b"small\n", True)
        client.eof()
    print("CASE P04-valid-invalid-second", flush=True)
    with contextlib.closing(Client(server.port)) as client:
        client.send(request() + request(headers=b"Content-Length: +1\r\n") + request())
        client.response(200, b"home\n")
        client.response(400, ERRORS[400], True)
        client.eof()
    print("CASE P01-head migrated-to-static-HEAD", flush=True)
    with contextlib.closing(Client(server.port)) as client:
        client.send(request(method=b"HEAD") + request(headers=b"Connection: close\r\n"))
        client.response(200, b"home\n", method=b"HEAD")
        client.response(200, b"home\n", True)
        client.eof()
    rejects = [
        ("P01-host-missing", b"GET / HTTP/1.1\r\n\r\n", 400),
        ("P01-host-duplicate", request(headers=b"Host: again\r\n"), 400),
        ("P01-method-invalid", request(method=b"G(ET"), 400),
        ("P01-post", request(method=b"POST"), 405),
        ("P02-post-bad-body", request(method=b"POST", headers=b"Content-Length: +1\r\n"), 400),
        ("P02-nonzero", request(headers=b"Content-Length: +1\r\n"), 400),
        ("P02-duplicate", request(headers=b"Content-Length: 0\r\nContent-Length: 0\r\n"), 400),
        ("P02-list", request(headers=b"Content-Length: 0,0\r\n"), 400),
        ("P02-negative", request(headers=b"Content-Length: -1\r\n"), 400),
        ("P02-te", request(headers=b"Transfer-Encoding: chunked, chunked\r\n"), 400),
        ("P02-cl-te", request(headers=b"Content-Length: 0\r\nTransfer-Encoding: chunked\r\n"), 400),
        ("P02-expect", request(headers=b"Expect: 100-continue\r\nExpect: 100-continue\r\n"), 400),
    ]
    for case_id, wire, status in rejects:
        check_exchange(server, case_id, wire + request(), status, ERRORS[status])
    check_exchange(server, "P04-empty-tokens", request(headers=b"Connection: , , close\r\n"), 200, b"home\n")


def check_disconnects(server):
    for prefix in [b"", b"GET / HTTP/1.1\r\nHost:"]:
        print("CASE P06-fin-" + str(len(prefix)), flush=True)
        with contextlib.closing(Client(server.port)) as client:
            client.send(prefix)
            client.socket.shutdown(socket.SHUT_WR)
            client.response(400, ERRORS[400], True)
            client.eof()
    print("CASE P06-rst-isolation", flush=True)
    with contextlib.closing(Client(server.port)) as client:
        client.send(request(b"/limit.dat"))
        client.socket.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    check_exchange(server, "P08-after-rst", request(headers=b"Connection: close\r\n"), 200, b"home\n")


def check_timeouts(binary, root, threads, logs):
    with running_server(binary, root, threads, logs, idle=150, keep_alive=0) as server:
        print("CASE P06-idle-timeout", flush=True)
        with contextlib.closing(Client(server.port)) as client:
            client.send(b"GET / HTTP/1.1\r\nHost:")
            client.eof()
    with running_server(binary, root, threads, logs, idle=0, keep_alive=150) as server:
        print("CASE P06-keepalive-timeout", flush=True)
        with contextlib.closing(Client(server.port)) as client:
            client.send(request())
            client.response(200, b"home\n")
            client.eof()


def interrupt_suite(signum, frame):
    raise TimeoutError("120 second suite deadline")


def main():
    binary = str(Path(sys.argv[1]).resolve())
    temporary_root = Path(os.environ.get("HP_S3_TEST_TMP_ROOT", tempfile.gettempdir()))
    temporary_root.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="http-baseline-", dir=temporary_root))
    logs = run / "logs"
    logs.mkdir()
    root = run / "www"
    root.mkdir()
    (root / "index.html").write_bytes(b"home\n")
    (root / "small.txt").write_bytes(b"small\n")
    binary_body = bytes(range(256)) * 4
    large_body = bytes(range(256)) * (8 * 1024 * 1024 // 256)
    (root / "binary.dat").write_bytes(binary_body)
    (root / "limit.dat").write_bytes(large_body)
    with (root / "over.dat").open("wb") as output:
        output.truncate(8 * 1024 * 1024 + 1)
    (run / "outside").write_bytes(b"must not escape\n")
    (root / "link").symlink_to(run / "outside")
    (root / "dirlink").symlink_to(run, target_is_directory=True)
    signal.signal(signal.SIGALRM, interrupt_suite)
    signal.alarm(120)
    passed = False
    try:
        for threads in [0, 2]:
            print("WORKERS", threads, flush=True)
            with running_server(binary, root, threads, logs) as server:
                check_static(server, binary_body, large_body)
                check_protocol(server)
                check_disconnects(server)
            check_timeouts(binary, root, threads, logs)
        passed = True
    finally:
        signal.alarm(0)
        shutil.rmtree(root)
        (run / "outside").unlink()
        (logs / "summary.json").write_text(json.dumps({"passed": passed, "fixtures_removed": not root.exists()}))
        print("EVIDENCE", logs, "fixtures_removed", not root.exists(), flush=True)
    print("HTTP process baseline PASS", flush=True)


if __name__ == "__main__":
    main()
