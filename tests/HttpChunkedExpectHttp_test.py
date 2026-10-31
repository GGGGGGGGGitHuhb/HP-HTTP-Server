#!/usr/bin/env python3
"""V1.2/S1: bounded chunked decoding and explicit 100/final lifecycle."""
import contextlib
import json
import os
from pathlib import Path
import select
import shutil
import signal
import socket
import struct
import sys
import tempfile
import time

from HttpProtocolBaselineHttp_test import Client, request, require, interrupt_suite
from HttpRequestBodyHttp_test import running_server, read_metrics, wait_readable
from HttpMethodRoutingHttp_test import echo_request

ERRORS = {status: f"{status} {phrase}\n".encode() for status, phrase in
          [(400, "Bad Request"), (405, "Method Not Allowed"), (413, "Content Too Large"),
           (417, "Expectation Failed"), (501, "Not Implemented")]}
CHUNK = b"Transfer-Encoding: chunked\r\n"
EXPECT = b"Expect: 100-continue\r\n"


def chunk_body(body):
    if not body:
        return b"0\r\n\r\n"
    return f"{len(body):X}".encode() + b'; ext="secret-extension-marker"\r\n' + body + b"\r\n0\r\nX-Checksum: secret-trailer-marker\r\n\r\n"


def reject(server, fields, status, method=b"POST", path=b"/api/echo"):
    with contextlib.closing(Client(server.port)) as client:
        client.send(request(path, fields, method))
        client.response(status, ERRORS[status], True, method=method,
                        allow=b"POST" if path == b"/api/echo" else b"GET, HEAD")
        client.eof()


def check_handshake(server):
    for chunked in [False, True]:
        for body in [b"abc", b"a\0\xff\r\n", request(), bytes(range(256)) * 4096]:
            fields = CHUNK if chunked else b"Content-Length: " + str(len(body)).encode() + b"\r\n"
            with contextlib.closing(Client(server.port)) as client:
                client.send(request(b"/api/echo?private-query-marker", fields + EXPECT, b"POST"))
                client.continue_response()
                wire = chunk_body(body) if chunked else body
                for start in range(0, len(wire), 7919):
                    client.send(wire[start:start + 7919])
                expected = body + b"wrong-negative" if os.environ.get("HP_CHUNKED_EXPECT_NEGATIVE") else body
                client.response(200, expected)
                client.send(request(headers=b"Connection: close\r\n"))
                client.response(200, b"home\n", True)
                client.eof()
    print("CASE Q01-wait-100-CL-chunk-binary-1MiB PASS", flush=True)
    with contextlib.closing(Client(server.port)) as client:
        client.send(request(b"/api/echo", CHUNK + EXPECT, b"POST") + chunk_body(b"coalesced") +
                    echo_request(b"fixed") + request(method=b"HEAD") + request(headers=b"Connection: close\r\n"))
        client.continue_response()
        client.response(200, b"coalesced")
        client.response(200, b"fixed")
        client.response(200, b"home\n", method=b"HEAD")
        client.response(200, b"home\n", True)
        client.eof()
    for fields in [EXPECT, EXPECT + b"Content-Length: 0\r\n"]:
        with contextlib.closing(Client(server.port)) as client:
            client.send(request(b"/api/echo", fields + b"Connection: close\r\n", b"POST"))
            client.response(200, b"", True)
            client.eof()
    with contextlib.closing(Client(server.port)) as client:
        client.send(request(headers=EXPECT + CHUNK, method=b"HEAD"))
        client.continue_response()
        client.send(chunk_body(b"ignored") + request(headers=b"Connection: close\r\n"))
        client.response(200, b"home\n", method=b"HEAD")
        client.response(200, b"home\n", True)
        client.eof()
    print("CASE Q02-coalesced-once-no-body-HEAD-full-pipeline PASS", flush=True)


def check_headers(server):
    for fields, status in [(CHUNK + b"Content-Length:0\r\n", 400),
                           (CHUNK + CHUNK, 400), (b"Transfer-Encoding: chunked,chunked\r\n", 400),
                           (b"Transfer-Encoding: chunked,gzip\r\n", 400),
                           (b"Transfer-Encoding:gzip\r\n", 400),
                           (b"Transfer-Encoding:gzip,chunked\r\n" + EXPECT, 501),
                           (b"Expect:other\r\n", 417), (EXPECT + EXPECT, 400), (b"Expect:\r\n", 400),
                           (b"Content-Length:1048577\r\n" + EXPECT, 413),
                           (b"Content-Length:1048577\r\nExpect:other\r\n", 413),
                           (b"Content-Length:1048577\r\nExpect:other\r\nBad Header:x\r\n", 400),
                           (b"Transfer-Encoding:gzip,chunked\r\nExpect:other\r\nBad Header:x\r\n", 400)]:
        for method in [b"POST", b"HEAD"]:
            reject(server, fields, status, method)
    reject(server, CHUNK + EXPECT, 405, b"GET")
    reject(server, CHUNK + EXPECT, 405, b"HEAD")
    reject(server, CHUNK + EXPECT, 405, b"POST", b"/missing")
    reject(server, CHUNK + EXPECT, 400, b"POST", b"/api/echo?q=%2G")
    # 没有Expect时必须先消费并检查body；同一路由的非法chunk优先400。
    with contextlib.closing(Client(server.port)) as client:
        client.send(request(b"/api/echo", CHUNK, b"GET") + b"!\r\n" + request())
        client.response(400, ERRORS[400], True)
        client.eof()
    print("CASE Q03-head-only-errors-admission-priority PASS", flush=True)


def check_trailers_and_fin(server):
    fields = CHUNK + b"Trailer: X-Checksum, X-Trace-Id\r\n"
    with contextlib.closing(Client(server.port)) as client:
        client.send(request(b"/api/echo", fields, b"POST") + b'1 ; x = "a\\\"b"\r\nz\r\n00;ok\r\nX-Checksum:a\r\nx-checksum:b\r\nX-Trace-Id:c\r\n\r\n' + echo_request(b"next", True))
        client.response(200, b"z")
        client.response(200, b"next", True)
        client.eof()
    for field in [b"Host", b"Content-Length", b"Transfer-Encoding", b"Connection", b"Expect", b"Target", b"Other"]:
        with contextlib.closing(Client(server.port)) as client:
            client.send(request(b"/api/echo", CHUNK, b"POST") + b"0\r\n" + field + b": evil\r\n\r\n" + request())
            client.response(400, ERRORS[400], True)
            client.eof()
    for prefix in [b"", b"1", b"1\r", b"1\r\n", b"1\r\nx", b"1\r\nx\r", b"0\r\n", b"0\r\nX-Checksum:x\r\n"]:
        with contextlib.closing(Client(server.port)) as client:
            client.send(request(b"/api/echo", CHUNK, b"POST") + prefix)
            client.socket.shutdown(socket.SHUT_WR)
            client.response(400, ERRORS[400], True)
            client.eof()
    print("CASE Q04-trailer-policy-and-FIN-prefixes PASS", flush=True)


def require_deadline_close(sock):
    import errno
    try:
        data = sock.recv(1)
    except ConnectionResetError as error:
        if error.errno != errno.ECONNRESET:
            raise
        return 'RST'
    require(data == b"", "deadline unexpectedly sent final/data")
    return 'EOF'


def check_drip_stage(server, prefix, case):
    with contextlib.closing(Client(server.port)) as client:
        started = time.monotonic()
        print("CASE Q05-enter", case, flush=True)
        if case == "header":
            client.send(b"POST /api/echo HTTP/1.1\r\nHost:")
        else:
            client.send(request(b"/api/echo", CHUNK + EXPECT, b"POST"))
            client.continue_response()
            client.send(prefix)
        sent = 0
        closed = False
        while time.monotonic() - started < 0.9:
            ready, _, _ = select.select([client.socket], [], [], 0.04)
            if ready:
                observation = require_deadline_close(client.socket)
                print("CASE Q05-close", case, observation, flush=True)
                closed = True
                break
            client.send(b"x")
            sent += 1
        elapsed = time.monotonic() - started
        require(closed and 0.18 <= elapsed < 0.8 and sent >= 3, "absolute read deadline was renewed")
        print("CASE Q05-" + case, "seconds", round(elapsed, 4), "progress_writes", sent, flush=True)
    with contextlib.closing(Client(server.port)) as healthy:
        healthy.send(echo_request(b"healthy", True))
        healthy.response(200, b"healthy", True)
        healthy.eof()


def check_metrics(binary, root, threads, logs):
    with running_server(binary, root, threads, logs) as server:
        with contextlib.closing(Client(server.port)) as client:
            client.send(request(b"/api/echo?private-query-marker", CHUNK + EXPECT, b"POST"))
            client.continue_response()
            client.send(chunk_body(b"secret-body-marker"))
            client.response(200, b"secret-body-marker")
        reject(server, b"Expect:other\r\n", 417)
        reject(server, b"Transfer-Encoding:gzip,chunked\r\n", 501)
        reject(server, CHUNK + EXPECT, 405, b"HEAD")
        check_drip_stage(server, b'1;x="', "size-extension")
    metrics, text = read_metrics(server)
    require(metrics["responses_completed_total"] == 5 and metrics["requests_aborted_total"] == 1, "100/final accounting")
    require(metrics["responses_status_417_total"] == 1 and metrics["responses_status_501_total"] == 1, "tail status buckets")
    require(metrics["responses_status_unknown_total"] == 1 and metrics["parse_errors_total"] == 2, "timeout/parse counters")
    records = [json.loads(line[line.index('{'):]) for line in text.splitlines() if '"content_bytes"' in line]
    require(len(records) == 6 and all(r["status"] != 100 for r in records), "interim access record")
    require(any(r["status"] == 405 and r["content_bytes"] == 0 for r in records), "HEAD final body counter")
    for secret in ["secret-body-marker", "secret-extension-marker", "secret-trailer-marker", "private-query-marker"]:
        require(secret not in text, "payload/metadata leak")
    print("CASE Q06-metrics-final-only-unknown-abort-tail-buckets PASS", flush=True)


def check_disabled_and_shutdown(binary, root, threads, logs):
    with running_server(binary, root, threads, logs, read_timeout=0) as server:
        with contextlib.closing(Client(server.port)) as client:
            client.send(request(b"/api/echo", CHUNK + EXPECT, b"POST"))
            client.continue_response()
            require(not wait_readable(client, 0.4), "disabled deadline responded before body")
            client.send(chunk_body(b"done") + request(headers=b"Connection: close\r\n"))
            client.response(200, b"done")
            client.response(200, b"home\n", True)
            client.eof()
        reset = Client(server.port)
        reset.send(request(b"/api/echo", CHUNK + EXPECT, b"POST"))
        reset.continue_response()
        reset.socket.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        reset.close()
        with contextlib.closing(Client(server.port)) as pending:
            pending.send(request(b"/api/echo", CHUNK + EXPECT, b"POST"))
            pending.continue_response()
            server.stop()
            pending.eof()
    read_metrics(server)
    print("CASE Q07-zero-timeout-RST-interim-SIGTERM PASS", flush=True)


def main():
    binary = str(Path(sys.argv[1]).resolve())
    parent = Path(os.environ["HP_S3_TEST_TMP_ROOT"])
    parent.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="http-chunked-", dir=parent))
    root, logs = run / "www", run / "logs"
    root.mkdir()
    logs.mkdir()
    (root / "index.html").write_bytes(b"home\n")
    signal.signal(signal.SIGALRM, interrupt_suite)
    signal.alarm(120)
    passed = False
    try:
        for threads in [0, 2]:
            print("WORKERS", threads, flush=True)
            with running_server(binary, root, threads, logs, read_timeout=2000) as server:
                check_handshake(server)
                check_headers(server)
                check_trailers_and_fin(server)
            with running_server(binary, root, threads, logs) as server:
                for prefix, case in [(b"", "header"), (b'1;x="', "size-extension"), (b"100\r\n", "data"), (b"0\r\nX-Checksum:", "trailer")]:
                    check_drip_stage(server, prefix, case)
            check_metrics(binary, root, threads, logs)
            check_disabled_and_shutdown(binary, root, threads, logs)
        passed = True
    finally:
        signal.alarm(0)
        shutil.rmtree(root)
        (logs / "summary.json").write_text(json.dumps({"passed": passed, "fixtures_removed": not root.exists()}))
        print("EVIDENCE", logs, "fixtures_removed", not root.exists(), flush=True)
    print("Chunked Expect HTTP baseline PASS", flush=True)


if __name__ == "__main__":
    main()
