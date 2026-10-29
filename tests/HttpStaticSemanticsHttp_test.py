#!/usr/bin/env python3
"""V1.2/S3 state-aware static responses, bounded sockets and fixture ownership."""
import contextlib
from email.utils import format_datetime, parsedate_to_datetime
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import sys
import tempfile
import time

from HttpProtocolBaselineHttp_test import Client, request, require, interrupt_suite, MAX_RESPONSE
from HttpRequestBodyHttp_test import running_server, read_metrics, wait_readable
from HttpChunkedExpectHttp_test import CHUNK, EXPECT, chunk_body

DATA = bytes(range(251)) * 20000  # 5,020,000 bytes, larger than typical socket send buffers.
RANGE_ERROR = b"416 Range Not Satisfiable\n"
MTIME = 784111777


class StaticClient(Client):
    def response(self, status, expected_body, close=False, method=b"GET", content_range=None, static=True):
        while b"\r\n\r\n" not in self.pending:
            self.receive()
        raw, self.pending = self.pending.split(b"\r\n\r\n", 1)
        lines = raw.split(b"\r\n")
        require(lines[0].split(b" ")[1] == str(status).encode(), f"wrong status {lines[0]!r}, wanted {status}")
        headers = {}
        for line in lines[1:]:
            name, value = line.split(b":", 1)
            name = name.lower()
            require(name not in headers, "duplicate header")
            headers[name] = value.strip()
        require(headers[b"connection"] == (b"close" if close else b"keep-alive"), "connection policy")
        require(headers.get(b"content-range") == content_range, "Content-Range mismatch")
        if status == 304:
            require(b"content-length" not in headers and b"content-type" not in headers and expected_body == b"", "304 framing")
            length = 0
        else:
            require(headers[b"content-length"].isdigit(), "invalid CL")
            length = int(headers[b"content-length"])
            require(length == len(expected_body) and length <= MAX_RESPONSE, "CL mismatch")
            require(b"content-type" in headers, "missing Content-Type")
        if static:
            require(headers[b"etag"].startswith(b'W/"') and headers[b"etag"].endswith(b'"'), "weak ETag")
            require(headers[b"accept-ranges"] == b"bytes", "Accept-Ranges")
            for name in (b"date", b"last-modified"):
                value = headers[name].decode("ascii")
                require(format_datetime(parsedate_to_datetime(value), usegmt=True) == value, "invalid date")
            require(parsedate_to_datetime(headers[b"last-modified"].decode()) <= parsedate_to_datetime(headers[b"date"].decode()), "future Last-Modified")
        else:
            require(not any(name in headers for name in (b"etag", b"date", b"last-modified", b"accept-ranges")), "unexpected static metadata")
        if method == b"HEAD":
            length, expected_body = 0, b""
        while len(self.pending) < length:
            self.receive()
        body, self.pending = self.pending[:length], self.pending[length:]
        require(body == expected_body, "body mismatch")
        return headers


def exchange(server, headers=b"", status=200, body=DATA, method=b"GET", path=b"/data.bin", content_range=None, static=True):
    with contextlib.closing(StaticClient(server.port)) as client:
        client.send(request(path, headers + b"Connection: close\r\n", method))
        result = client.response(status, body, True, method, content_range, static)
        client.eof()
        return result


def check_cache(server, root):
    baseline = exchange(server)
    tag = baseline[b"etag"]
    require(exchange(server)[b"etag"] == tag, "unstable ETag")
    ims = baseline[b"last-modified"]
    for fields in (b"If-None-Match: *\r\n", b"If-None-Match: " + tag + b"\r\n",
                   b"If-None-Match: " + tag[2:] + b"\r\n", b'If-None-Match: "x,y"\r\nIf-None-Match: ' + tag + b"\r\n",
                   b"If-Modified-Since: " + ims + b"\r\n",
                   b"If-Modified-Since: Sunday, 06-Nov-94 08:49:37 GMT\r\n",
                   b"If-Modified-Since: Sun Nov  6 08:49:37 1994\r\n"):
        for method in (b"GET", b"HEAD"):
            exchange(server, fields, 304, b"", method)
    for invalid in (b"", b"bad", tag + b",", b"*, " + tag, b'"bad space", ' + tag):
        exchange(server, b"If-None-Match: " + invalid + b"\r\nIf-Modified-Since: " + ims + b"\r\n", method=b"HEAD")
    exchange(server, b"If-Modified-Since: " + ims + b"\r\nIf-Modified-Since: " + ims + b"\r\n", method=b"HEAD")
    # Replacement changes inode despite identical content and explicit old mtime.
    temporary = root / "replacement"
    temporary.write_bytes(DATA)
    os.utime(temporary, (MTIME, MTIME))
    temporary.replace(root / "data.bin")
    require(exchange(server, method=b"HEAD")[b"etag"] != tag, "replacement ETag unchanged")
    os.utime(root / "future", (4102444800, 4102444800))
    future = exchange(server, body=b"future", path=b"/future")
    require(future[b"last-modified"] == future[b"date"], "future mtime not clamped")
    print("CASE H01-validators-three-dates-precedence-replacement-future PASS", flush=True)


def check_ranges(server):
    expected = b"wrong!!" if os.environ.get("HP_STATIC_SEMANTICS_NEGATIVE") else DATA[123:130]
    exchange(server, b"Range: bytes=123-129\r\n", 206, expected, content_range=b"bytes 123-129/5020000")
    for wire, start, end in [(b"0-0", 0, 0), (b"0003-0005", 3, 5), (b"123-", 123, len(DATA)-1),
                              (b"-99", len(DATA)-99, len(DATA)-1), (b"-9999999", 0, len(DATA)-1),
                              (b"5000000-18446744073709551615", 5000000, len(DATA)-1)]:
        content_range = f"bytes {start}-{end}/{len(DATA)}".encode()
        exchange(server, b"Range: BYTES=" + wire + b"\r\n", 206, DATA[start:end+1], content_range=content_range)
    for wire in (b"bytes=-0", b"bytes=5020000-", b"bytes=18446744073709551615-"):
        exchange(server, b"Range: " + wire + b"\r\n", 416, RANGE_ERROR, content_range=b"bytes */5020000")
    exchange(server, b"Range: bytes=0-0\r\n", 416, RANGE_ERROR, path=b"/empty", content_range=b"bytes */0")
    for wire in (b"bytes=2-1", b"items=0-1", b"bytes=0-1,2-3", b"bytes=0-1,", b"bytes=+1-2",
                 b"bytes=1 -2", b"bytes=18446744073709551616-", b"bytes=0-1\r\nRange: bytes=2-3"):
        exchange(server, b"Range: " + wire + b"\r\n")
    tag = exchange(server, method=b"HEAD")[b"etag"]
    for validator in (tag, tag[2:], b'"different"', b"", b"bad", b"Sun, 06 Nov 1994 08:49:37 GMT",
                      b"Wed, 01 Jan 2020 00:00:00 GMT", b"x\r\nIf-Range: y"):
        exchange(server, b"Range: bytes=2-5\r\nIf-Range: " + validator + b"\r\n")
    for wire in (b"bytes=1-3", b"bytes=9999999-", b"bytes=bad"):
        exchange(server, b"Range: " + wire + b"\r\n", method=b"HEAD")
    exchange(server, b"Range: bytes=9-\r\nIf-Range: bad\r\nIf-None-Match: *\r\n", 304, b"")
    print("CASE H02-single-ranges-large-offset-bytes-IfRange-HEAD PASS", flush=True)


def check_pipeline(server):
    with contextlib.closing(StaticClient(server.port)) as client:
        client.send(request(b"/data.bin", b"If-None-Match: *\r\n") +
                    request(b"/data.bin", b"Range: bytes=13-42\r\n") +
                    request(b"/data.bin", b"Range: bytes=0-0\r\n", b"HEAD") +
                    request(b"/data.bin", b"Range: bytes=-0\r\n") +
                    request(b"/empty", b"Connection: close\r\n"))
        client.response(304, b"")
        client.response(206, DATA[13:43], content_range=b"bytes 13-42/5020000")
        client.response(200, DATA, method=b"HEAD")
        client.response(416, RANGE_ERROR, content_range=b"bytes */5020000")
        client.response(200, b"", True)
        client.eof()
    # Both framing modes wait for the complete body after one interim response.
    for chunked in (False, True):
        for condition, status, body, cr in [(b"If-None-Match: *\r\n", 304, b"", None),
                                          (b"Range: bytes=-0\r\n", 416, RANGE_ERROR, b"bytes */5020000")]:
            with contextlib.closing(StaticClient(server.port)) as client:
                client.send(request(b"/data.bin", condition + EXPECT + (CHUNK if chunked else b"Content-Length: 3\r\n")))
                client.continue_response()
                require(not client.pending, "final before body")
                client.send((chunk_body(b"abc") if chunked else b"abc") + request(b"/empty", b"Connection: close\r\n"))
                client.response(status, body, content_range=cr)
                client.response(200, b"", True)
                client.eof()
    with contextlib.closing(StaticClient(server.port)) as client:
        client.send(request(b"/data.bin", b"If-None-Match: *\r\n"))
        client.socket.shutdown(socket.SHUT_WR)
        client.response(304, b"")
        client.eof()
    print("CASE H03-304-206-HEAD-416-GET-pipeline-Expect-FIN PASS", flush=True)


def check_errors(server):
    for path, status in ((b"/missing", 404), (b"/link", 403), (b"/large", 403), (b"/../x", 403)):
        phrase = b"Not Found" if status == 404 else b"Forbidden"
        exchange(server, b"If-None-Match: *\r\n", status, str(status).encode()+b" "+phrase+b"\n", path=path, static=False)
    exchange(server, b"If-None-Match: *\r\nContent-Length: +1\r\n", 400, b"400 Bad Request\n", static=False)
    exchange(server, b"If-None-Match: *\r\n", 405, b"405 Method Not Allowed\n", method=b"FROB", static=False)
    with contextlib.closing(StaticClient(server.port)) as client:
        client.send(request(b"/api/echo", b"If-None-Match: *\r\nRange: bytes=0-0\r\nContent-Length: 4\r\nConnection: close\r\n", b"POST")+b"echo")
        client.response(200, b"echo", True, static=False)
        client.eof()
    print("CASE H04-file-target-framing-method-priority-echo PASS", flush=True)


def check_observability(binary, root, threads, logs):
    with running_server(binary, root, threads, logs, read_timeout=2000) as server:
        exchange(server, b'If-None-Match: "private-condition-value"\r\n', method=b"HEAD", path=b"/data.bin?private-query-key=value")
        exchange(server, b"If-None-Match: *\r\n", 304, b"")
        exchange(server, b"Range: bytes=3-5\r\n", 206, DATA[3:6], content_range=b"bytes 3-5/5020000")
        exchange(server, b"Range: bytes=-0\r\n", 416, RANGE_ERROR, content_range=b"bytes */5020000")
    metrics, text = read_metrics(server)
    require(metrics["responses_completed_total"] == metrics["requests_started_total"] == 4, "final counts")
    for status in (200, 206, 304, 416):
        require(metrics[f"responses_status_{status}_total"] == 1, "status counters")
    require(metrics["errors_total"] == 1 and metrics["parse_errors_total"] == metrics["provider_errors_total"] == 0, "error attribution")
    records = [json.loads(line[line.index('{'):]) for line in text.splitlines() if '"content_bytes"' in line]
    require(len(records) == 4 and [record["content_bytes"] for record in records] == [0, 0, 3, 26], "content_bytes")
    require("private-condition-value" not in text and "private-query-key" not in text, "sensitive log data")
    print("CASE H05-status-metrics-content-bytes-no-secret-drained PASS", flush=True)


def check_body_deadline(binary, root, threads, logs):
    with running_server(binary, root, threads, logs, read_timeout=300) as server:
        with contextlib.closing(StaticClient(server.port)) as client:
            client.send(request(b"/data.bin", b"If-None-Match: *\r\nContent-Length: 3\r\n") + b"a")
            require(not wait_readable(client, 0.03), "conditional final before complete body")
            client.send(b"bc" + request(b"/empty", b"Connection: close\r\n"))
            client.response(304, b"")
            client.response(200, b"", True)
            client.eof()
        with contextlib.closing(StaticClient(server.port)) as client:
            started = time.monotonic()
            client.send(request(b"/data.bin", b"If-None-Match: *\r\nContent-Length: 100\r\n" + EXPECT))
            client.continue_response()
            writes, closed = 0, False
            while time.monotonic() - started < 0.8:
                if wait_readable(client, 0.04):
                    client.eof()
                    closed = True
                    break
                client.send(b"x")
                writes += 1
            elapsed = time.monotonic() - started
            require(closed and 0.18 <= elapsed < 0.8 and writes >= 3, "conditional request deadline reset")
            print("DEADLINE", round(elapsed, 4), "progress_writes", writes, flush=True)
        with contextlib.closing(StaticClient(server.port)) as client:
            client.send(request(b"/data.bin", b"If-None-Match: *\r\nContent-Length: 3\r\n") + b"a")
            client.socket.shutdown(socket.SHUT_WR)
            client.response(400, b"400 Bad Request\n", True, static=False)
            client.eof()
    read_metrics(server)
    print("CASE H06-condition-body-gating-deadline-truncated-FIN PASS", flush=True)


def main():
    binary = str(Path(sys.argv[1]).resolve())
    parent = Path(os.environ["HP_S3_TEST_TMP_ROOT"])
    parent.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="http-static-", dir=parent))
    root, logs = run / "www", run / "logs"
    root.mkdir(); logs.mkdir()
    (root / "data.bin").write_bytes(DATA)
    os.utime(root / "data.bin", (MTIME, MTIME))
    (root / "empty").write_bytes(b"")
    (root / "future").write_bytes(b"future")
    with (root / "large").open("wb") as file:
        file.truncate(8 * 1024 * 1024 + 1)
    (root / "link").symlink_to(root / "data.bin")
    signal.signal(signal.SIGALRM, interrupt_suite)
    signal.alarm(120)
    passed = False
    try:
        for threads in (0, 2):
            print("WORKERS", threads, flush=True)
            with running_server(binary, root, threads, logs, read_timeout=2000) as server:
                check_ranges(server)
                check_cache(server, root)
                check_pipeline(server)
                check_errors(server)
            check_observability(binary, root, threads, logs)
            check_body_deadline(binary, root, threads, logs)
        passed = True
    finally:
        signal.alarm(0)
        shutil.rmtree(root)
        (logs / "summary.json").write_text(json.dumps({"passed": passed, "fixtures_removed": not root.exists()}))
        print("EVIDENCE", logs, "fixtures_removed", not root.exists(), flush=True)
    print("Static semantics HTTP PASS", flush=True)


if __name__ == "__main__":
    main()
