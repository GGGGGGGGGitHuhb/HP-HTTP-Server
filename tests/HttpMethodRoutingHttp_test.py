#!/usr/bin/env python3
"""S3: exact routing, HEAD metadata, binary echo, ordered persistent HTTP."""
import contextlib
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import sys
import struct
import tempfile

from HttpProtocolBaselineHttp_test import Client, require, request, interrupt_suite
from HttpRequestBodyHttp_test import running_server, read_metrics, check_drip, wait_readable

ERRORS = {code: f"{code} {name}\n".encode() for code, name in
          [(400, "Bad Request"), (403, "Forbidden"), (404, "Not Found"),
           (405, "Method Not Allowed"), (413, "Content Too Large")]}


def echo_request(body=b"", close=False, path=b"/api/echo"):
    headers = b"Content-Length: " + str(len(body)).encode() + b"\r\n"
    if close:
        headers += b"Connection: close\r\n"
    return request(path, headers, b"POST") + body


def exchange(server, wire, status, representation, method=b"GET", allow=b"GET, HEAD", eof=False):
    with contextlib.closing(Client(server.port)) as client:
        client.send(wire)
        if eof:
            client.socket.shutdown(socket.SHUT_WR)
        headers = client.response(status, representation, True, method=method, allow=allow)
        client.eof()
        return headers


def check_echo(server):
    for body in [b"", b"x", b"a\0\xff\r\n", request(method=b"HEAD"), bytes(range(256)) * 4096]:
        print("CASE E01-echo", len(body), flush=True)
        with contextlib.closing(Client(server.port)) as client:
            wire = echo_request(body, path=b"/api/echo?private-query-marker")
            for start in range(0, len(wire), 7919):
                client.send(wire[start:start + 7919])
            client.send(request(headers=b"Connection: close\r\n"))
            expected = body + b"wrong-negative" if os.environ.get("HP_METHOD_ROUTING_NEGATIVE") else body
            headers = client.response(200, expected)
            require(headers[b"content-type"] == b"application/octet-stream", "echo content type")
            client.response(200, b"home\n", True)
            client.eof()
    exchange(server, request(b"/api/echo", b"Connection: close\r\n", b"POST"), 200, b"")


def check_routes(server):
    for method in [b"GET", b"HEAD", b"POST", b"get", b"FROB"]:
        for path in [b"/api/echo", b"/api/echo?q=plain"]:
            status = 200 if method == b"POST" else 405
            exchange(server, request(path, b"Connection: close\r\n", method) + request(),
                     status, b"" if status == 200 else ERRORS[405], method, b"POST")
        for path, static_status in [(b"/missing", 404), (b"/api/missing", 404), (b"/api/echo/", 403), (b"/", 200)]:
            status = static_status if method in (b"GET", b"HEAD") else 405
            exchange(server, request(path, b"Connection: close\r\n", method) + request(),
                     status, b"home\n" if status == 200 else ERRORS[status], method)
        exchange(server, request(b"/api/echo?q=%20", method=method) + request(), 400, ERRORS[400], method)
    print("CASE E02-route-query-percent-shadow-tail-allow-close PASS", flush=True)


def check_head(server):
    for path, body, status in [(b"/", b"home\n", 200), (b"/empty", b"", 200),
                               (b"/large", b"\0" * (8 * 1024 * 1024), 200),
                               (b"/link", ERRORS[403], 403), (b"/missing", ERRORS[404], 404)]:
        with contextlib.closing(Client(server.port)) as client:
            client.send(request(path, method=b"HEAD") + echo_request(b"after-head") + request(headers=b"Connection: close\r\n"))
            head = client.response(status, body, method=b"HEAD")
            client.response(200, b"after-head")
            get = client.response(200, b"home\n", True)
            client.eof()
        if path == b"/":
            require(head[b"content-type"] == get[b"content-type"], "HEAD content type mismatch")
    for wire, status, eof, trusted in [
        (b"HEAD / HTTP/1.1\r\n\r\n", 400, False, True),
        (request(headers=b"Content-Length: 1048577\r\n", method=b"HEAD"), 413, False, True),
        (request(headers=b"Content-Length: 2\r\n", method=b"HEAD") + b"x", 400, True, True),
        (b"HEAD / HTTP/1.0\r\n", 400, False, False)]:
        exchange(server, wire, status, ERRORS[status], b"HEAD" if trusted else b"GET", eof=eof)
    print("CASE E03-HEAD-success-errors-pipeline-no-extra-bytes PASS", flush=True)


def check_rejections(server):
    for headers, status in [(b"Content-Length: 1048577\r\n", 413),
                            (b"Content-Length: +1\r\n", 400),
                            (b"Content-Length: 1\r\nContent-Length: 1\r\n", 400),
                            (b"Transfer-Encoding: chunked\r\n", 400),
                            (b"Expect: 100-continue\r\n", 400)]:
        exchange(server, request(b"/api/echo", headers, b"POST") + request(), status, ERRORS[status])
    exchange(server, request(b"/api/echo", b"Content-Length: 2\r\n", b"POST") + b"x", 400, ERRORS[400], eof=True)
    with contextlib.closing(Client(server.port)) as client:
        client.send(echo_request(b"ordered") + request(method=b"HEAD") + request() + request(b"/missing", b"Connection: close\r\n"))
        client.response(200, b"ordered")
        client.response(200, b"home\n", method=b"HEAD")
        client.response(200, b"home\n")
        client.response(404, ERRORS[404], True)
        client.eof()
    print("CASE E04-rejection-priority-and-mixed-order PASS", flush=True)


def check_metrics(binary, root, threads, logs):
    with running_server(binary, root, threads, logs, read_timeout=2000) as server:
        exchange(server, echo_request(b"secret-echo-body-marker", True, b"/api/echo?private-query-marker"), 200, b"secret-echo-body-marker")
        exchange(server, request(headers=b"Connection: close\r\n", method=b"HEAD"), 200, b"home\n", b"HEAD")
        exchange(server, request(method=b"FROB"), 405, ERRORS[405])
        exchange(server, request(headers=b"Content-Length: +1\r\n"), 400, ERRORS[400])
        exchange(server, request(headers=b"Content-Length: 1048577\r\n"), 413, ERRORS[413])
    metrics, text = read_metrics(server)
    require(metrics["requests_started_total"] == metrics["responses_completed_total"] == 5, "completion counts")
    require(metrics["parse_errors_total"] == 2 and metrics["errors_total"] == 3, "405 must not count as parse error")
    require(metrics["responses_status_405_total"] == 1, "405 bucket")
    require("secret-echo-body-marker" not in text and "private-query-marker" not in text, "body/query leakage")
    records = [json.loads(line[line.index('{'):]) for line in text.splitlines() if '"content_bytes"' in line]
    require(any(r.get("method") == "HEAD" and r["content_bytes"] == 0 for r in records), "HEAD access bytes")
    require(any(r.get("method") == "POST" and r["content_bytes"] == len(b"secret-echo-body-marker") for r in records), "echo access bytes")
    print("CASE E05-observability PASS", flush=True)


def check_shutdown(binary, root, threads, logs):
    with running_server(binary, root, threads, logs, read_timeout=0) as server:
        with contextlib.closing(Client(server.port)) as client:
            client.send(request(b"/api/echo", b"Content-Length: 3\r\n", b"POST") + b"a")
            require(not wait_readable(client, 0.4), "disabled read deadline responded early")
            client.send(b"bc" + request(headers=b"Connection: close\r\n", method=b"HEAD"))
            client.response(200, b"abc")
            client.response(200, b"home\n", True, method=b"HEAD")
            client.eof()
        reset = Client(server.port)
        reset.send(request(b"/api/echo", b"Content-Length: 1000\r\n", b"POST") + b"partial")
        reset.socket.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        reset.close()
        exchange(server, echo_request(b"healthy", True), 200, b"healthy")
        with contextlib.closing(Client(server.port)) as pending:
            pending.send(request(b"/api/echo", b"Content-Length: 1000\r\n", b"POST") + b"incomplete")
            # witness 是真实已处理事件，避免用 sleep 推断另一个连接已经进入循环。
            exchange(server, echo_request(b"witness", True), 200, b"witness")
            server.stop()
    read_metrics(server)
    print("CASE E07-echo-zero-timeout-RST-drain PASS", flush=True)


def main():
    binary = str(Path(sys.argv[1]).resolve())
    parent = Path(os.environ["HP_S3_TEST_TMP_ROOT"])
    parent.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="http-method-", dir=parent))
    root, logs = run / "www", run / "logs"
    root.mkdir()
    logs.mkdir()
    (root / "index.html").write_bytes(b"home\n")
    (root / "api").mkdir()
    (root / "api/echo").write_bytes(b"static-shadow-must-not-be-sent")
    (root / "empty").write_bytes(b"")
    with (root / "large").open("wb") as output:
        output.truncate(8 * 1024 * 1024)
    (root / "link").symlink_to(root / "index.html")
    signal.signal(signal.SIGALRM, interrupt_suite)
    signal.alarm(120)
    passed = False
    try:
        for threads in [0, 2]:
            print("WORKERS", threads, flush=True)
            with running_server(binary, root, threads, logs, read_timeout=2000) as server:
                check_echo(server)
                check_routes(server)
                check_head(server)
                check_rejections(server)
            check_metrics(binary, root, threads, logs)
            check_shutdown(binary, root, threads, logs)
            with running_server(binary, root, threads, logs) as server:
                check_drip(server, request(b"/api/echo", b"Content-Length: 1000\r\n", b"POST"), "E06-echo-absolute-deadline")
        passed = True
    finally:
        signal.alarm(0)
        shutil.rmtree(root)
        (logs / "summary.json").write_text(json.dumps({"passed": passed, "fixtures_removed": not root.exists()}))
        print("EVIDENCE", logs, "fixtures_removed", not root.exists(), flush=True)
    print("Method routing HTTP baseline PASS", flush=True)


if __name__ == "__main__":
    main()
