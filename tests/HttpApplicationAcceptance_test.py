#!/usr/bin/env python3
"""真实组合验收；复用只读客户端，所有新启动资源由本驱动回收。"""
import concurrent.futures
import contextlib
import importlib.util
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
import threading
import time

from HttpProtocolBaselineHttp_test import Client, require, request, interrupt_suite
from HttpTaskApiHttp_test import TaskClient, task_wire, encoded, error, exchange
from HttpStaticSemanticsHttp_test import StaticClient
from HttpAsyncTaskHttp_test import Fixture, start_task


@contextlib.contextmanager
def fixture(binary, root, threads, logs, **options):
    # 显式保留部分初始化对象；旧夹具负责进程清理，新驱动负责完整证据。
    server = Fixture.__new__(Fixture)
    original = None
    try:
        Fixture.__init__(server, binary, root, threads, logs, **options)
        yield server
        server.stop()
    except BaseException as failure:
        original = failure
        if not getattr(server, "cleaned", False):
            server.cleanup(validate=False)
        raise
    finally:
        process = getattr(server, "process", None)
        reader = getattr(server, "reader", None)
        record = {"pid": process.pid if process else None,
                  "port": getattr(server, "port", None),
                  "returncode": process.returncode if process else None,
                  "reaped": process is None or process.returncode is not None,
                  "reader_joined": reader is None or not reader.is_alive(),
                  "pipes_closed": process is None or (process.stdin.closed and process.stdout.closed),
                  "original_error": repr(original) if original else None,
                  "cleanup_errors": getattr(server, "cleanup_errors", []),
                  "listen_address": "0.0.0.0", "client_address": "127.0.0.1",
                  "forced": process is not None and process.returncode == -signal.SIGKILL,
                  "root_owner": "application suite finally", "log": str(getattr(server, "path", ""))}
        try:
            server.path.with_suffix(".json").write_text(json.dumps(record, indent=2))
        except BaseException as save_error:
            if original is not None:
                original.add_note("gate evidence: " + repr(save_error))
            else:
                raise


class ApplicationClient(TaskClient, StaticClient):
    pass


def check_pipeline(server):
    with contextlib.closing(ApplicationClient(server.port)) as client:
        initial = {"title": "管线任务", "completed": False}
        client.send(request() + task_wire(b"POST", body=encoded(initial)))
        headers = client.response(200, b"home")
        created = client.task_response(201)
        require(created["title"] == initial["title"] and created["completed"] is False, "created fields")
        identifier = created["id"]
        require(type(identifier) is str and identifier.isdecimal(), "returned ID")
        path = b"/api/tasks/" + identifier.encode()
        replacement = {"title": "完整替换", "completed": True}
        changed = {"id": identifier, **replacement}
        client.send(task_wire(path=path) + task_wire(b"PUT", path, encoded(replacement)) +
                    task_wire(path=path) + task_wire(b"DELETE", path) + task_wire(path=path) +
                    request(b"/api/echo", b"Content-Length: 3\r\n", b"POST") + b"a\x00b" +
                    request(method=b"HEAD") +
                    request(headers=b"If-None-Match: " + headers[b"etag"] + b"\r\n") +
                    request(headers=b"Range: bytes=1-2\r\nConnection: close\r\n"))
        client.task_response(200, created)
        client.task_response(200, changed)
        client.task_response(200, changed)
        client.task_response(200, {"id": identifier, "deleted": True})
        client.task_response(404, error("task_not_found"))
        client.response(200, b"a\x00b", static=False)
        client.response(200, b"home", method=b"HEAD")
        client.response(304, b"")
        client.response(206, b"om", close=True, content_range=b"bytes 1-2/4")
        client.eof()
    if os.environ.get("HP_APPLICATION_NEGATIVE"):
        require(False, "intentional mixed-response wrong expectation")
    print("CASE S303 static/JSON/echo/HEAD/304/206 ordered pipeline PASS", flush=True)


def modify_and_observe(server, barrier, identifier, producer):
    barrier.wait(timeout=5)
    path = b"/api/tasks/" + identifier.encode()
    for operation in range(32):
        complete = bool(producer % 2)
        value = {"title": "done" if complete else "pending", "completed": complete}
        expected = {"id": identifier, **value}
        exchange(server, b"PUT", path, encoded(value), expected=expected)
        observed = exchange(server, path=path)
        listing = exchange(server)["tasks"]
        require(observed["title"] == ("done" if observed["completed"] else "pending"), "torn item")
        require(len(listing) == 1 and listing[0]["id"] == identifier, "snapshot identity")
        require(listing[0]["title"] == ("done" if listing[0]["completed"] else "pending"), "torn list")
    return 32


def check_concurrent_http(server):
    created = exchange(server, b"POST", body=encoded({"title": "pending", "completed": False}), status=201)
    barrier = threading.Barrier(8)
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as workers:
        futures = [workers.submit(modify_and_observe, server, barrier, created["id"], producer)
                   for producer in range(8)]
        require(sum(future.result(timeout=30) for future in futures) == 256, "finite operation count")
    exchange(server, b"DELETE", b"/api/tasks/" + created["id"].encode(),
             expected={"id": created["id"], "deleted": True})
    exchange(server, expected={"tasks": []})
    print("CASE S304 real HTTP 8x32 whole-object PUT/GET/list PASS", flush=True)


def static_echo(port):
    with contextlib.closing(Client(port)) as client:
        client.send(request() + request(b"/api/echo", b"Content-Length: 4\r\nConnection: close\r\n", b"POST") + b"echo")
        client.response(200, b"home")
        client.response(200, b"echo", True)
        client.eof()


def check_slow_composition(binary, root, threads, logs):
    with fixture(binary, root, threads, logs, timeout=2000) as server:
        with contextlib.closing(start_task(server, task_wire() + request(headers=b"Connection: close\r\n"))) as slow:
            server.event("ENTERED")
            static_echo(server.port)
            require(not select.select([slow.socket], [], [], 0)[0], "pending pipeline suffix escaped")
            server.release()
            slow.task_response(200, {"tasks": []})
            slow.response(200, b"home", True)
            slow.eof()
        with contextlib.closing(TaskClient(server.port)) as client:
            body = encoded({"title": "Expect+FIN", "completed": False})
            client.send(request(b"/api/tasks", b"Expect: 100-continue\r\nContent-Type: application/json\r\nContent-Length: " +
                                str(len(body)).encode() + b"\r\n", b"POST"))
            client.continue_response()
            client.send(body)
            client.socket.shutdown(socket.SHUT_WR)
            created = client.task_response(201)
            require(created["title"] == "Expect+FIN", "FIN lost complete request")
            client.eof()
    print("CASE S305 slow job + independent static/echo + ordered suffix + Expect/FIN", threads, "PASS", flush=True)


def check_timeout_composition(binary, root, threads, logs):
    with fixture(binary, root, threads, logs, timeout=200, commit=True) as server:
        with contextlib.ExitStack() as clients:
            first = clients.enter_context(contextlib.closing(start_task(server, task_wire(b"POST", body=encoded({"title": "committed", "completed": True})))))
            server.event("ENTERED")
            second = clients.enter_context(contextlib.closing(start_task(server)))
            server.event("ENTERED")
            queued = clients.enter_context(contextlib.closing(start_task(server, task_wire(b"POST", body=encoded({"title": "cancelled-queue", "completed": False})))))
            static_echo(server.port)
            for client in (first, second, queued):
                client.task_response(503, {"error": "task_timeout"}, True)
                client.eof()
            server.release()
        tasks = exchange(server)["tasks"]
        require(len(tasks) == 1 and tasks[0]["title"] == "committed" and tasks[0]["completed"] is True,
                "timeout rollback/queued side effects")
        for _ in range(8):
            static_echo(server.port)
    print("CASE S306 committed timeout + cancelled queue + late-result connections", threads, "PASS", flush=True)


def check_disconnect_shutdown(binary, root, threads, logs):
    with fixture(binary, root, threads, logs, timeout=2000) as server:
        with contextlib.ExitStack() as clients:
            reset = clients.enter_context(contextlib.closing(start_task(server)))
            server.event("ENTERED")
            pending = clients.enter_context(contextlib.closing(start_task(server)))
            server.event("ENTERED")
            queued = clients.enter_context(contextlib.closing(start_task(server)))
            reset.socket.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            reset.close()
            # 制造连接关闭/重新分配压力；不伪称客户端能证明服务端具体FD已复用。
            for _ in range(16):
                static_echo(server.port)
            server.process.send_signal(signal.SIGTERM)
            server.event("NETWORK_STOPPED")
            require(server.process.poll() is None, "business join ended before gate release")
            server.process.send_signal(signal.SIGTERM)
            for client in (pending, queued):
                client.eof()
            server.release()
    print("CASE S307 disconnect/replacement + pending queue + two signals + gate join", threads, "PASS", flush=True)


def assert_cleanup(server):
    record = server.record
    require(record["reaped"] and record["reader_joined"] and record["pipes_closed"], "startup resource leak")
    require(not record["cleanup_errors"] and record["reader_error"] is None, "cleanup errors")
    require(server.process.poll() is not None and not server.reader.is_alive(), "live resource")
    require(server.log_path.with_suffix(".json").is_file(), "cleanup record missing")


def check_failure_cleanup(demo, binary, logs):
    records = []
    for mode in ("before-port", "after-port", "during-run"):
        marker = RuntimeError("injected application " + mode)
        root = Path(tempfile.mkdtemp(prefix="failure-static-", dir=logs))
        (root / "index.html").write_bytes(b"home")
        server = None
        original = None
        rescue = False

        class StartupFailure(demo.DemoServer):
            def wait_port(self):
                if mode == "after-port":
                    self.port = super().wait_port()
                raise marker

        try:
            if mode == "during-run":
                server = demo.DemoServer(binary, root, logs)
                with server:
                    require(demo.exchange(server, "GET", "/") == b"home", "real running precondition")
                    raise marker
            else:
                server = StartupFailure.__new__(StartupFailure)
                StartupFailure.__init__(server, binary, root, logs)
        except BaseException as error_value:
            original = error_value
        finally:
            if server is not None and (server.process.poll() is None or server.reader.is_alive()):
                rescue = True
                server.cleanup()
            shutil.rmtree(root)
        require(original is marker, "original failure was replaced")
        require(not rescue, "fixture required outer rescue")
        assert_cleanup(server)
        require(not root.exists(), "failure root remains")
        records.append({"mode": mode, "outer_rescue": rescue, "root_removed": not root.exists(), **server.record})
    (logs / "failure-cleanup.json").write_text(json.dumps(records, indent=2))
    print("CASE S308 before/after PORT and running failure own cleanup PASS", flush=True)


def load_demo(path):
    spec = importlib.util.spec_from_file_location("application_demo", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    binary, gate_binary, demo_path = sys.argv[1:]
    demo = load_demo(demo_path)
    base = Path(os.environ["HP_S3_TEST_TMP_ROOT"])
    base.mkdir(parents=True, exist_ok=True)
    logs = Path(tempfile.mkdtemp(prefix="application-evidence-", dir=base))
    root = None
    records = []
    passed = False
    original = None
    signal.signal(signal.SIGTERM, interrupt_suite)
    signal.signal(signal.SIGALRM, interrupt_suite)
    signal.alarm(120)
    print(demo.LISTEN_NOTICE, flush=True)
    try:
        root = Path(tempfile.mkdtemp(prefix="application-static-", dir=base))
        (root / "index.html").write_bytes(b"home")
        check_failure_cleanup(demo, binary, logs)
        for threads in (0, 2):
            with demo.DemoServer(binary, root, logs, threads) as server:
                records.append(server)
                check_pipeline(server)
                check_concurrent_http(server)
            check_slow_composition(gate_binary, root, threads, logs)
            check_timeout_composition(gate_binary, root, threads, logs)
            check_disconnect_shutdown(gate_binary, root, threads, logs)
        result = demo.demonstrate(binary, logs / "demo")
        require(result["passed"] and result["root_removed"], "demo unsuccessful")
        passed = True
    except BaseException as error_value:
        original = error_value
        raise
    finally:
        summary = {"passed": passed, "original_error": repr(original) if original else None,
                   "listen_address": "0.0.0.0", "client_address": "127.0.0.1",
                   "servers": [server.record for server in records]}
        try:
            demo.finalize_output(logs, root, summary, original)
        finally:
            signal.alarm(0)
            print("APPLICATION_SUMMARY", logs / "summary.json", flush=True)



if __name__ == "__main__":
    main()
