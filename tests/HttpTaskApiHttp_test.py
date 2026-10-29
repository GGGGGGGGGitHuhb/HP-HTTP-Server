#!/usr/bin/env python3
"""Task API: independent JSON expectations, bounded concurrency and owned cleanup."""
import contextlib
import concurrent.futures
import json
import os
from pathlib import Path
import shutil
import signal
import sys
import tempfile
import threading

from HttpProtocolBaselineHttp_test import Client, request, require, interrupt_suite
from HttpRequestBodyHttp_test import running_server, read_metrics, wait_readable
from HttpChunkedExpectHttp_test import chunk_body, CHUNK, EXPECT

ERRORS = {
    "invalid_id": "Invalid task identifier", "route_not_found": "Route not found",
    "unsupported_query": "Query parameters are not supported", "method_not_allowed": "Method not allowed",
    "body_too_large": "Task body exceeds 8192 bytes", "unexpected_body": "Request body is not allowed",
    "unsupported_media_type": "Content-Type must be application/json", "invalid_json": "Invalid task JSON",
    "task_not_found": "Task not found", "capacity_exhausted": "Task capacity exhausted",
    "id_exhausted": "Task identifiers exhausted", "internal_error": "Internal error"}


def encoded(value):
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def error(code):
    return {"error": {"code": code, "message": ERRORS[code]}}


def task_wire(method=b"GET", path=b"/api/tasks", body=b"", headers=b"", media=True):
    if media and method in (b"POST", b"PUT"):
        headers += b"Content-Type: application/json\r\n"
    return request(path, headers + b"Content-Length: " + str(len(body)).encode() + b"\r\n", method) + body


class TaskClient(Client):
    def task_response(self, status, expected=None, close=False, head=False, allow=None):
        while b"\r\n\r\n" not in self.pending:
            self.receive()
        raw, self.pending = self.pending.split(b"\r\n\r\n", 1)
        lines = raw.split(b"\r\n")
        require(lines[0].split(b" ")[1] == str(status).encode(), f"wrong status {lines[0]!r}; wanted {status}")
        headers = {}
        for line in lines[1:]:
            name, value = line.split(b":", 1)
            name = name.lower()
            require(name not in headers, "duplicate header")
            headers[name] = value.strip()
        require(headers[b"cache-control"] == b"no-store", "no-store missing")
        require(headers[b"content-type"] == b"application/json", "JSON content type")
        require(headers.get(b"allow") == allow, "Allow mismatch")
        require(headers[b"connection"] == (b"close" if close else b"keep-alive"), "connection policy")
        require(headers[b"content-length"].isdigit(), "invalid CL")
        length = int(headers[b"content-length"])
        require(0 < length <= 262144, "API response budget")
        if head:
            require(length == len(encoded(expected)), "HEAD representation length")
            return None
        while len(self.pending) < length:
            self.receive()
        body, self.pending = self.pending[:length], self.pending[length:]
        require(not body.endswith(b"\n"), "extra JSON newline")
        result = json.loads(body)
        if expected is not None:
            require(result == expected, f"wrong JSON {result!r}; wanted {expected!r}")
        return result


def exchange(server, method=b"GET", path=b"/api/tasks", body=b"", status=200, expected=None,
             headers=b"", media=True, close=False, allow=None):
    with contextlib.closing(TaskClient(server.port)) as client:
        client.send(task_wire(method, path, body, headers, media))
        result = client.task_response(status, expected, close, method == b"HEAD", allow)
        if close:
            client.eof()
        return result


def assert_task(value, title, completed):
    require(set(value) == {"id", "title", "completed"} and type(value["id"]) is str and
            value["id"].isdigit() and int(value["id"]) > 0, "task shape/id")
    require(type(value["completed"]) is bool and value["completed"] == completed and value["title"] == title, "task fields")


def delete(server, identifier):
    return exchange(server, b"DELETE", b"/api/tasks/" + identifier.encode(), expected={"id": identifier, "deleted": True})


def check_crud(server):
    exchange(server, expected={"tasks": []})
    created = exchange(server, b"POST", body=encoded({"title": "first", "completed": False}), status=201)
    assert_task(created, "wrong" if os.environ.get("HP_TASK_API_NEGATIVE") else "first", False)
    identifier = created["id"]
    require(identifier == "1", "fresh instance ID")
    exchange(server, path=b"/api/tasks/1", expected=created)
    exchange(server, path=b"/%61pi/tasks/%31?", expected=created)
    changed = exchange(server, b"PUT", b"/api/tasks/1", encoded({"completed": True, "title": "replacement"}))
    assert_task(changed, "replacement", True)
    require(changed["id"] == identifier, "PUT changed ID")
    exchange(server, expected={"tasks": [changed]})
    exchange(server, b"PUT", b"/api/tasks/1", b'{"title":"partial"}', 400, error("invalid_json"))
    exchange(server, path=b"/api/tasks/1", expected=changed)
    delete(server, identifier)
    exchange(server, b"DELETE", b"/api/tasks/1", status=404, expected=error("task_not_found"))
    exchange(server, expected={"tasks": []})
    print("CASE B01-CRUD-complete-PUT-owned-encoded-no-side-effects PASS", flush=True)


def check_routes(server):
    for path, code, status in [(b"/api/tasks/0", "invalid_id", 400), (b"/api/tasks/01", "invalid_id", 400),
                              (b"/api/tasks/+1", "invalid_id", 400), (b"/api/tasks/-1", "invalid_id", 400),
                              (b"/api/tasks/x", "invalid_id", 400), (b"/api/tasks/18446744073709551616", "invalid_id", 400),
                              (b"/api/tasks/1/more", "route_not_found", 404), (b"/api/tasks?flag", "unsupported_query", 400)]:
        for method in (b"GET", b"HEAD", b"FROB"):
            exchange(server, method, path, status=status, expected=error(code))
    for path in (b"/api/tasks/18446744073709551615", b"/api/tasks/999"):
        exchange(server, path=path, status=404, expected=error("task_not_found"))
    for path, allow in [(b"/api/tasks", b"GET, POST"), (b"/api/tasks/999", b"GET, PUT, DELETE")]:
        for method in (b"HEAD", b"OPTIONS", b"PATCH", b"get"):
            with contextlib.closing(TaskClient(server.port)) as client:
                client.send(task_wire(method, path) + task_wire())
                client.task_response(405, error("method_not_allowed"), True, method == b"HEAD", allow)
                client.eof()
    # 400/404/415 errors without client close must preserve the next response.
    for wire, status, code in [(task_wire(path=b"/api/tasks?x"), 400, "unsupported_query"),
                               (task_wire(path=b"/api/tasks/999"), 404, "task_not_found"),
                               (task_wire(b"POST", body=b"{}", media=False), 415, "unsupported_media_type")]:
        with contextlib.closing(TaskClient(server.port)) as client:
            client.send(wire + task_wire(headers=b"Connection: close\r\n"))
            client.task_response(status, error(code))
            client.task_response(200, {"tasks": []}, True)
            client.eof()
    for path, status, body in [(b"/api/tasks/", 403, b"403 Forbidden\n"),
                               (b"/api/tasks?bad=%GG", 400, b"400 Bad Request\n"),
                               (b"/api/tasksX", 200, b"fallback"), (b"/api/missing", 404, b"404 Not Found\n")]:
        with contextlib.closing(Client(server.port)) as client:
            client.send(request(path, b"Connection: close\r\n"))
            headers = client.response(status, body, True)
            require(b"cache-control" not in headers, "old response changed")
            client.eof()
    print("CASE B02-route-ID-query-HEAD-Allow-error-keepalive-old-fallback PASS", flush=True)


def check_inputs(server):
    invalid = [b"", b"null", b"[]", b"{}", b'{"title":"x"}', b'{"completed":false}',
               b'{"title":"","completed":false}', b'{"title":"x","completed":1}',
               b'{"title":null,"completed":false}', b'{"title":"x","completed":null}',
               b'{"title":"x","completed":false,"extra":0}', b'{"title":"x","title":"y","completed":false}',
               b'{"title":"x","tit\\u006ce":"y","completed":false}', b'{"title":{},"completed":false}',
               b'{"title":[],"completed":false}', b'{"title":"\\uD800","completed":false}',
               b'{"title":"\\u0000","completed":false}', b'{"title":"\\u007f","completed":false}',
               b'{"title":"\\n","completed":false}', b'{"title":"\xff","completed":false}',
               b'{"title":"x","completed":false,}', b'{/*x*/"title":"x","completed":false}',
               b'[' * 4000 + b']' * 4000]
    valid = encoded({"title": "x", "completed": False})
    invalid += [valid + b"{}", valid + b"\0", b"\xef\xbb\xbf" + valid, encoded({"title": "x" * 257, "completed": False})]
    for body in invalid:
        exchange(server, b"POST", body=body, status=400, expected=error("invalid_json"))
    exchange(server, expected={"tasks": []})
    for media in [b"", b"text/plain", b"application/json;", b"application/json; charset=ascii",
                  b"application/json; x=y", b"application/json; charset=utf-8; charset=utf-8",
                  b"application/json\r\nContent-Type: application/json"]:
        exchange(server, b"POST", body=valid, headers=b"Content-Type: " + media + b"\r\n", media=False,
                 status=415, expected=error("unsupported_media_type"))
    for media in [b"Application/JSON", b'application/json ; Charset = "UTF-8"']:
        value = exchange(server, b"POST", body=valid, status=201, media=False, headers=b"Content-Type: " + media + b"\r\n")
        delete(server, value["id"])
    for title in ["x" * 256, "é" * 128, 'quote" slash\\ 中文', " "]:
        value = exchange(server, b"POST", body=encoded({"title": title, "completed": True}), status=201)
        assert_task(value, title, True)
        delete(server, value["id"])
    padded = valid + b" " * (8192 - len(valid))
    for chunked in (False, True):
        with contextlib.closing(TaskClient(server.port)) as client:
            wire = (request(b"/api/tasks", CHUNK + b"Content-Type: application/json\r\n", b"POST") + chunk_body(padded)) if chunked else task_wire(b"POST", body=padded)
            client.send(wire)
            value = client.task_response(201)
        delete(server, value["id"])
        with contextlib.closing(TaskClient(server.port)) as client:
            wire = (request(b"/api/tasks", CHUNK + b"Content-Type: application/json\r\n", b"POST") + chunk_body(padded+b" ")) if chunked else task_wire(b"POST", body=padded+b" ")
            client.send(wire + task_wire())
            client.task_response(413, error("body_too_large"), True)
            client.eof()
    for method in (b"GET", b"DELETE"):
        exchange(server, method, body=b"x", path=b"/api/tasks" if method == b"GET" else b"/api/tasks/999", status=400, expected=error("unexpected_body"))
    exchange(server, b"PUT", b"/api/tasks/999", valid, 404, error("task_not_found"))
    exchange(server, expected={"tasks": []})
    print("CASE B03-JSON-media-8KiB-chunked-UTF8-deep-no-mutation PASS", flush=True)


def check_expect(server):
    valid = encoded({"title": "expect", "completed": False})
    for chunked in (False, True):
        with contextlib.closing(TaskClient(server.port)) as client:
            framing = CHUNK if chunked else b"Content-Length: " + str(len(valid)).encode() + b"\r\n"
            client.send(request(b"/api/tasks", EXPECT + framing + b"Content-Type: application/json\r\n", b"POST"))
            client.continue_response()
            exchange(server, expected={"tasks": []})
            client.send(chunk_body(valid) if chunked else valid)
            created = client.task_response(201)
        delete(server, created["id"])
    with contextlib.closing(TaskClient(server.port)) as client:
        wire = task_wire(b"POST", body=valid)
        client.send(wire[:-1])
        require(not wait_readable(client, 0.03), "committed before complete body")
        exchange(server, expected={"tasks": []})
        client.close()
    exchange(server, expected={"tasks": []})
    with contextlib.closing(TaskClient(server.port)) as client:
        client.send(request(b"/api/tasks/0", EXPECT + b"Content-Length: 1\r\n", b"POST") + task_wire(headers=b"Connection: close\r\n"))
        # Early route rejection never emits 100 or mutates storage. The existing session closes rejected heads.
        client.task_response(400, error("invalid_id"), close=True)
        client.eof()
    exchange(server, expected={"tasks": []})
    print("CASE B04-Expect-and-incomplete-body-no-side-effect-disconnect PASS", flush=True)


def concurrent_create(server, barrier, number):
    barrier.wait(timeout=5)
    value = exchange(server, b"POST", body=encoded({"title": "worker-" + str(number), "completed": False}), status=201)
    assert_task(value, "worker-" + str(number), False)
    return value


def check_concurrency(server):
    barrier = threading.Barrier(8)
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as workers:
        futures = [workers.submit(concurrent_create, server, barrier, number) for number in range(8)]
        values = [future.result(timeout=5) for future in futures]
    require(len({value["id"] for value in values}) == 8, "duplicate concurrent ID")
    result = exchange(server)["tasks"]
    require(result == sorted(values, key=lambda value: int(value["id"])), "shared ordered snapshot")
    for value in values:
        delete(server, value["id"])
    print("CASE B05-eight-connections-shared-store-unique-joined PASS", flush=True)


def check_capacity(binary, root, threads, logs):
    title = '\\"' * 128
    with running_server(binary, root, threads, logs, read_timeout=2000) as server:
        tasks = []
        for number in range(128):
            value = exchange(server, b"POST", body=encoded({"title": title, "completed": False}), status=201)
            require(value["id"] == str(number + 1), "capacity IDs")
            tasks.append(value)
        with contextlib.closing(TaskClient(server.port)) as client:
            client.send(task_wire(b"POST", body=encoded({"title": "overflow", "completed": False})) + task_wire())
            client.task_response(503, error("capacity_exhausted"))
            client.task_response(200, {"tasks": tasks})
        require(len(encoded({"tasks": tasks})) <= 213008 < 262144, "worst escaped list budget")
        delete(server, "1")
        value = exchange(server, b"POST", body=encoded({"title": "private-title", "completed": True}), status=201)
        require(value["id"] == "129", "deleted ID reused")
        exchange(server, b"POST", body=b"{}", media=False, status=415, expected=error("unsupported_media_type"))
        exchange(server, path=b"/api/tasks?private-query=secret", status=400, expected=error("unsupported_query"))
        exchange(server, b"HEAD", status=405, expected=error("method_not_allowed"), close=True, allow=b"GET, POST")
    metrics, text = read_metrics(server)
    require(metrics["responses_status_201_total"] == 129 and metrics["responses_status_415_total"] == 1 and metrics["responses_status_503_total"] == 1, "new status buckets")
    require(metrics["errors_total"] == 4 and metrics["parse_errors_total"] == metrics["provider_errors_total"] == 0, "API error attribution")
    records = [json.loads(line[line.index('{'):]) for line in text.splitlines() if '"content_bytes"' in line]
    require(len(records) == metrics["responses_completed_total"] == 135, "one final/access")
    require(records[-1]["method"] == "HEAD" and records[-1]["content_bytes"] == 0, "HEAD bytes")
    require(records[0]["content_bytes"] == len(encoded(tasks[0])), "JSON bytes")
    require("private-title" not in text and "private-query" not in text and "overflow" not in text, "sensitive logging")
    print("CASE B06-capacity128-503-keepalive-list-bound-16metrics PASS", flush=True)


def check_admission(binary, root, threads, logs):
    cases = [(b"/api/tasks/0", 400, "invalid_id"),
             (b"/api/tasks?x", 400, "unsupported_query"),
             (b"/api/tasks/1/more", 404, "route_not_found")]
    with running_server(binary, root, threads, logs, read_timeout=2000) as server:
        for path, status, code in cases:
            for method in (b"GET", b"HEAD"):
                with contextlib.closing(TaskClient(server.port)) as client:
                    client.send(request(path, EXPECT + b"Content-Length: 1\r\n", method))
                    client.task_response(status, error(code), True, method == b"HEAD")
                    client.eof()
        with contextlib.closing(TaskClient(server.port)) as client:
            client.send(request(b"/api/tasks/0", EXPECT + b"Content-Length: 1\r\n", b"POST") + b"x" +
                        task_wire(b"POST", body=encoded({"title": "must-not-commit", "completed": False})))
            client.task_response(400, error("invalid_id"), True)
            client.eof()
        for path, status, code in cases:
            for method in (b"GET", b"HEAD"):
                with contextlib.closing(TaskClient(server.port)) as client:
                    client.send(task_wire(method, path, b"x") + task_wire(headers=b"Connection: close\r\n"))
                    client.task_response(status, error(code), head=method == b"HEAD")
                    client.task_response(200, {"tasks": []}, True)
                    client.eof()
        with contextlib.closing(TaskClient(server.port)) as client:
            client.send(request(b"/api/tasks", EXPECT + b"Content-Length: 1\r\nContent-Type: application/json\r\n", b"POST"))
            client.continue_response()
            client.send(b"!" + task_wire(headers=b"Connection: close\r\n"))
            client.task_response(400, error("invalid_json"))
            client.task_response(200, {"tasks": []}, True)
            client.eof()
        created = exchange(server, b"POST", body=encoded({"title": "first", "completed": False}), status=201)
        require(created["id"] == "1", "head rejection allocated ID")
        delete(server, "1")
    metrics, text = read_metrics(server)
    require(metrics["requests_started_total"] == metrics["responses_completed_total"] == 23, "head rejection final count")
    require(metrics["errors_total"] == 14 and metrics["parse_errors_total"] == metrics["provider_errors_total"] == 0, "head rejection attribution")
    records = [json.loads(line[line.index('{'):]) for line in text.splitlines() if '"content_bytes"' in line]
    require(len(records) == 23 and all(r["content_bytes"] == 0 for r in records if r["method"] == "HEAD"), "head rejection access accounting")
    require("must-not-commit" not in text, "head rejection log secret")
    print("CASE B07-Expect-head-close-vs-complete-keepalive-HEAD-no-effects PASS", flush=True)


def main():
    binary = str(Path(sys.argv[1]).resolve())
    parent = Path(os.environ["HP_S3_TEST_TMP_ROOT"]); parent.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="http-tasks-", dir=parent)); root, logs = run / "www", run / "logs"
    (root / "api/tasks").mkdir(parents=True); logs.mkdir()
    (root / "index.html").write_bytes(b"home")
    (root / "api/tasks/1").write_bytes(b"shadow")
    (root / "api/tasksX").write_bytes(b"fallback")
    signal.signal(signal.SIGALRM, interrupt_suite); signal.alarm(120)
    passed = False
    try:
        for threads in (0, 2):
            print("WORKERS", threads, flush=True)
            with running_server(binary, root, threads, logs, read_timeout=2000) as server:
                check_crud(server); check_routes(server); check_inputs(server); check_expect(server); check_concurrency(server)
            require((root / "api/tasks/1").read_bytes() == b"shadow", "API changed file")
            with running_server(binary, root, threads, logs, read_timeout=2000) as restarted:
                exchange(restarted, expected={"tasks": []})
            check_capacity(binary, root, threads, logs)
            check_admission(binary, root, threads, logs)
        passed = True
    finally:
        signal.alarm(0); shutil.rmtree(root)
        (logs / "summary.json").write_text(json.dumps({"passed": passed, "fixtures_removed": not root.exists()}))
        print("EVIDENCE", logs, "fixtures_removed", not root.exists(), flush=True)
    print("Task API HTTP PASS", flush=True)


if __name__ == "__main__":
    main()
