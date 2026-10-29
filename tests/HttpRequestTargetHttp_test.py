#!/usr/bin/env python3
"""V1.2/S2 owned request targets, safe file resolution and query boundaries."""
import contextlib
import json
import os
from pathlib import Path
import shutil
import signal
import sys
import tempfile

from HttpProtocolBaselineHttp_test import Client, request, require, interrupt_suite
from HttpRequestBodyHttp_test import running_server, read_metrics
from HttpChunkedExpectHttp_test import CHUNK, EXPECT, chunk_body

ERRORS = {status: f"{status} {phrase}\n".encode() for status, phrase in
          [(400, "Bad Request"), (403, "Forbidden"), (404, "Not Found"), (405, "Method Not Allowed")]}


def exchange(server, path, status, body, method=b"GET", allow=b"GET, HEAD"):
    with contextlib.closing(Client(server.port)) as client:
        client.send(request(path, b"Connection: close\r\n", method) + request())
        headers = client.response(status, body, True, method=method, allow=allow)
        client.eof()
        return headers


def check_files_and_splits(server):
    expected = b"wrong-negative" if os.environ.get("HP_REQUEST_TARGET_NEGATIVE") else b"space\n"
    exchange(server, b"/space%20name.txt", 200, expected)
    for path, body in [(b"/space%20name.txt?q=%20", b"space\n"), (b"/high%FF.bin", b"high\n"),
                       (b"/high%ff.bin", b"high\n"), (b"/a..b", b"dots\n"),
                       (b"/?path=../outside/secret", b"home\n"), (b"/?q=a%26b%3Dc%3Fd&plus=+&q=%2526", b"home\n")]:
        get = exchange(server, path, 200, body)
        head = exchange(server, path, 200, body, b"HEAD")
        require(get == head, "GET/HEAD representation headers differ")
    wire = request(b"/space%20name.txt?q=%2526", b"Connection: close\r\n")
    # 覆盖请求行每个切分位置，不假定send边界等于recv边界。
    line_length = wire.index(b"\r\n") + 2
    for split in range(1, line_length):
        with contextlib.closing(Client(server.port)) as client:
            client.send(wire[:split])
            client.send(wire[split:])
            client.response(200, b"space\n", True)
            client.eof()
    print("CASE U01-encoded-files-high-byte-HEAD-and-wire-splits PASS", flush=True)


def check_rejections(server):
    bad = [b"/%2f", b"/%2F", b"/%5c", b"/%5C", b"/%3f", b"/%3F", b"/%23", b"/%25", b"/%252e%252e/secret",
           b"/%", b"/%1", b"/%GG", b"/%00", b"/%1f", b"/%7f", b"/?q=%00", b"/?q=%7F",
           b"/?=x", b"/?&a=x", b"/?a=x&", b"/?a&&b", b"/../secret?q=%GG"]
    forbidden = [b"/.", b"/..", b"/%2e", b"/%2E%2e/secret", b"/.%2e/secret", b"/a/./b", b"/a/../b",
                 b"/a\\b", b"//x", b"/a//b", b"/a/"]
    for status, paths in [(400, bad), (403, forbidden)]:
        for path in paths:
            for method in [b"GET", b"HEAD"]:
                exchange(server, path, status, ERRORS[status], method)
    for path in [b"/l%69nk", b"/dir%6cink/secret"]:
        exchange(server, path, 403, ERRORS[403])
    exchange(server, b"/m%69ssing", 404, ERRORS[404])
    for method in [b"POST", b"FROB"]:
        exchange(server, b"/missing?bad=%2G", 400, ERRORS[400], method)
        exchange(server, b"/%2e%2e/secret", 403, ERRORS[403], method)
        exchange(server, b"/m%69ssing", 405, ERRORS[405], method)
    print("CASE U02-path-query-priority-close-symlink-no-secret PASS", flush=True)


def check_query_limits(server):
    pairs = b"&".join([b"k"] * 64)
    raw_limit = b"a=" + b"x" * 1024 + b"&b=" + b"y" * 1019
    require(len(raw_limit) == 2048, "fixture raw query limit")
    for query in [b"", b"flag&a=&k=first&k=second", pairs, b"k" * 128,
                  b"k=" + b"v" * 1024, raw_limit]:
        exchange(server, b"/?" + query, 200, b"home\n")
    for query in [pairs + b"&k", b"k" * 129, b"k=" + b"v" * 1025, raw_limit + b"y"]:
        exchange(server, b"/?" + query, 400, ERRORS[400])
    print("CASE U03-query-exact-wire-bounds PASS", flush=True)


def check_router_and_expect(server):
    for method in [b"GET", b"HEAD", b"FROB"]:
        exchange(server, b"/%61pi/echo?q=%25", 405, ERRORS[405], method, b"POST")
    for chunked in [False, True]:
        fields = CHUNK if chunked else b"Content-Length: 3\r\n"
        with contextlib.closing(Client(server.port)) as client:
            client.send(request(b"/%61pi/echo?q=%2526", fields + EXPECT, b"POST"))
            client.continue_response()
            client.send((chunk_body(b"abc") if chunked else b"abc") +
                        request(b"/space%20name.txt", method=b"HEAD") + request(headers=b"Connection: close\r\n"))
            client.response(200, b"abc")
            client.response(200, b"space\n", method=b"HEAD")
            client.response(200, b"home\n", True)
            client.eof()
    for path, status in [(b"/%61pi/echo?bad=%2G", 400), (b"/%2e%2e/secret", 403)]:
        for method in [b"POST", b"HEAD"]:
            with contextlib.closing(Client(server.port)) as client:
                client.send(request(path, CHUNK + EXPECT, method) + request())
                client.response(status, ERRORS[status], True, method=method)
                client.eof()
    # 无Expect时正文分帧先于目标403。
    with contextlib.closing(Client(server.port)) as client:
        client.send(request(b"/%2e%2e/secret", CHUNK, b"POST") + b"!\r\n")
        client.response(400, ERRORS[400], True)
        client.eof()
    print("CASE U04-encoded-route-shadow-Expect-framing-priority-pipeline PASS", flush=True)


def check_metrics(binary, root, threads, logs):
    with running_server(binary, root, threads, logs, read_timeout=2000) as server:
        exchange(server, b"/space%20name.txt?private-query-key=private-query-value", 200, b"space\n", b"HEAD")
        with contextlib.closing(Client(server.port)) as client:
            client.send(request(b"/%61pi/echo?private-query-key=private-query-value", b"Content-Length: 14\r\nConnection: close\r\n", b"POST") + b"secret-payload")
            client.response(200, b"secret-payload", True)
            client.eof()
        exchange(server, b"/../?bad=%GG", 400, ERRORS[400])
        exchange(server, b"/../", 403, ERRORS[403])
        exchange(server, b"/bad%2G", 400, ERRORS[400])
        exchange(server, b"/missing?bad=%GG", 400, ERRORS[400], b"FROB")
        with contextlib.closing(Client(server.port)) as client:
            client.send(request(b"/../", b"Content-Length: +1\r\n"))
            client.response(400, ERRORS[400], True)
            client.eof()
    metrics, text = read_metrics(server)
    require(metrics["requests_started_total"] == metrics["responses_completed_total"] == 7, "request/final counters")
    require(metrics["parse_errors_total"] == 1 and metrics["provider_errors_total"] == 0 and metrics["errors_total"] == 5, "target errors must not become parser/provider errors")
    records = [json.loads(line[line.index('{'):]) for line in text.splitlines() if '"content_bytes"' in line]
    require(len(records) == 7 and any(r.get("method") == "HEAD" and r["content_bytes"] == 0 for r in records), "HEAD/access counts")
    require("/space%20name.txt" in text and "/%61pi/echo" in text, "raw path must remain raw")
    for secret in ["private-query-key", "private-query-value", "secret-payload"]:
        require(secret not in text, "query or payload leaked")
    print("CASE U05-target-errors-metrics-raw-path-no-query-log PASS", flush=True)


def main():
    binary = str(Path(sys.argv[1]).resolve())
    parent = Path(os.environ["HP_S3_TEST_TMP_ROOT"])
    parent.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="http-target-", dir=parent))
    root, logs, outside = run / "www", run / "logs", run / "outside"
    root.mkdir()
    logs.mkdir()
    outside.mkdir()
    (root / "index.html").write_bytes(b"home\n")
    (root / "space name.txt").write_bytes(b"space\n")
    (root / os.fsdecode(b"high\xff.bin")).write_bytes(b"high\n")
    (root / "a..b").write_bytes(b"dots\n")
    (root / "api").mkdir()
    (root / "api/echo").write_bytes(b"shadow-must-not-leak")
    (outside / "secret").write_bytes(b"outside-secret-must-not-leak")
    (root / "link").symlink_to(outside / "secret")
    (root / "dirlink").symlink_to(outside, target_is_directory=True)
    signal.signal(signal.SIGALRM, interrupt_suite)
    signal.alarm(120)
    passed = False
    try:
        for threads in [0, 2]:
            print("WORKERS", threads, flush=True)
            with running_server(binary, root, threads, logs, read_timeout=2000) as server:
                check_files_and_splits(server)
                check_rejections(server)
                check_query_limits(server)
                check_router_and_expect(server)
            check_metrics(binary, root, threads, logs)
        passed = True
    finally:
        signal.alarm(0)
        shutil.rmtree(root)
        shutil.rmtree(outside)
        (logs / "summary.json").write_text(json.dumps({"passed": passed, "fixtures_removed": not root.exists() and not outside.exists()}))
        print("EVIDENCE", logs, "fixtures_removed", not root.exists() and not outside.exists(), flush=True)
    print("Request target HTTP baseline PASS", flush=True)


if __name__ == "__main__":
    main()
