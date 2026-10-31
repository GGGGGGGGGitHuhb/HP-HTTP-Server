#!/usr/bin/env python3
"""Real owner/worker gate, bounded admission and shutdown integration."""
import contextlib
import json
import os
from pathlib import Path
import queue
import select
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

from HttpProtocolBaselineHttp_test import Client, require, request, interrupt_suite
from HttpRequestBodyHttp_test import running_server, read_metrics
from HttpTaskApiHttp_test import TaskClient, task_wire, exchange, encoded, error, check_crud


class Fixture:
    def __init__(self, binary, root, threads, logs, timeout=1000, commit=False):
        self.events = queue.Queue()
        self.lines = []
        self.path = logs / f"gate-{time.monotonic_ns()}.log"
        self.process = None
        self.reader = None
        self.reader_error = None
        self.released = False
        self.cleaned = False
        self.cleanup_errors = []
        try:
            self.process = subprocess.Popen([binary, str(root), str(threads), str(timeout),
                                             "commit" if commit else "before"], stdin=subprocess.PIPE,
                                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.reader = threading.Thread(target=self.collect)
            self.reader.start()
            self.port = int(self.event("PORT").split()[1])
        except BaseException as error:
            # Construction has not yielded to the context manager yet: we own cleanup here.
            self.cleanup(validate=False)
            if self.cleanup_errors:
                error.add_note("fixture cleanup: " + "; ".join(self.cleanup_errors))
            raise

    def collect(self):
        try:
            for raw in iter(self.process.stdout.readline, b""):
                line = raw.decode(errors="replace").strip()
                self.lines.append(line)
                self.events.put(line)
        except BaseException as error:
            self.reader_error = repr(error)

    def event(self, prefix):
        deadline = time.monotonic() + 5
        while True:
            line = self.events.get(timeout=max(0.001, deadline - time.monotonic()))
            if line.startswith(prefix):
                return line
            require(time.monotonic() < deadline, "fixture event deadline")

    def release(self):
        if not self.released:
            self.released = True
            self.process.stdin.write(b"release\n")
            self.process.stdin.flush()

    def cleanup(self, validate):
        if self.cleaned:
            return
        errors = self.cleanup_errors

        def attempt(action):
            try:
                return action()
            except BaseException as error:
                errors.append(repr(error))
                return None

        process = self.process
        if process is not None:
            if process.poll() is None:
                attempt(lambda: process.send_signal(signal.SIGTERM))
            # A broken/closed stdin must not bypass wait, kill, join or pipe closing.
            attempt(self.release)
            if process.poll() is None:
                attempt(lambda: process.wait(timeout=1))
            if process.poll() is None:
                attempt(process.kill)
                attempt(lambda: process.wait(timeout=1))
            else:
                attempt(process.wait)
        if self.reader is not None and self.reader.ident is not None:
            attempt(lambda: self.reader.join(timeout=1))
        if process is not None:
            for pipe in (process.stdin, process.stdout):
                if pipe is not None:
                    attempt(pipe.close)
        attempt(lambda: self.path.write_text("\n".join(self.lines)))
        self.cleaned = True
        if validate:
            require(process is not None and process.returncode == 0, "fixture exit failure")
            require(self.reader is not None and not self.reader.is_alive(), "fixture reader leak")
            require(not errors and self.reader_error is None, f"fixture cleanup errors: {errors}")
            final = [line.split() for line in self.lines if line.startswith("FINAL ")]
            require(len(final) == 1 and final[0][1] == "0" and final[0][2] == final[0][3], "slot conservation")

    def stop(self):
        self.cleanup(validate=True)


@contextlib.contextmanager
def fixture(*args, **kwargs):
    server = Fixture(*args, **kwargs)
    try:
        yield server
    except BaseException:
        server.cleanup(validate=False)
        raise
    else:
        server.stop()


def start_task(server, wire=None):
    client = TaskClient(server.port)
    client.send(wire or task_wire())
    return client


def check_responsive(binary, root, threads, logs):
    with fixture(binary, root, threads, logs, timeout=2000) as server:
        with contextlib.closing(start_task(server, task_wire() + request(headers=b"Connection: close\r\n"))) as slow:
            server.event("ENTERED")
            with contextlib.closing(Client(server.port)) as other:
                other.send(request() + request(b"/api/echo", b"Content-Length: 3\r\nConnection: close\r\n", b"POST") + b"abc")
                other.response(200, b"home")
                other.response(200, b"abc", True)
                other.eof()
            require(not select.select([slow.socket], [], [], 0)[0], "pipeline escaped pending task")
            server.release()
            slow.task_response(200, {"tasks": []})
            slow.response(200, b"home", True); slow.eof()
        with contextlib.closing(TaskClient(server.port)) as client:
            body = encoded({"title": "fin", "completed": False})
            client.send(request(b"/api/tasks", b"Expect: 100-continue\r\nContent-Type: application/json\r\nContent-Length: " + str(len(body)).encode() + b"\r\n", b"POST"))
            client.continue_response()
            client.send(body)
            client.socket.shutdown(socket.SHUT_WR)
            client.task_response(201)
            client.eof()
    print("CASE A01 responsive pipeline Expect FIN", threads, "PASS", flush=True)


def check_timeouts(binary, root, threads, logs):
    with fixture(binary, root, threads, logs, timeout=100, commit=True) as server:
        body = encoded({"title": "committed-before-timeout", "completed": False})
        with contextlib.ExitStack() as stack:
            first = stack.enter_context(contextlib.closing(start_task(server, task_wire(b"POST", body=body))))
            server.event("ENTERED")
            second = stack.enter_context(contextlib.closing(start_task(server)))
            server.event("ENTERED")
            queued = stack.enter_context(contextlib.closing(start_task(server, task_wire(b"POST", body=encoded({"title": "must-skip", "completed": False})))))
            for client in (first, second, queued):
                client.task_response(503, {"error": "task_timeout"}, True); client.eof()
            server.release()
        result = exchange(server)
        require([task["title"] for task in result["tasks"]] == ["committed-before-timeout"], "timeout side effect contract")
    print("CASE A02 queued skip/running commit timeout", threads, "PASS", flush=True)


def check_capacity(binary, root, logs):
    with fixture(binary, root, 0, logs, timeout=2000) as server:
        with contextlib.ExitStack() as stack:
            clients = []
            for _ in range(2):
                clients.append(stack.enter_context(contextlib.closing(start_task(server))))
                server.event("ENTERED")
            for _ in range(33):
                clients.append(stack.enter_context(contextlib.closing(start_task(server))))
            readable, _, _ = select.select([c.socket for c in clients], [], [], 1)
            require(len(readable) > 0, "overload not delivered")
            overloaded = next(c for c in clients if c.socket in readable)
            overloaded.task_response(503, {"error": "task_overloaded"}, True); overloaded.eof()
            server.release()
            for client in clients:
                if client is not overloaded:
                    client.task_response(200, {"tasks": []})
    print("CASE A03 34 outstanding/35th overload PASS", flush=True)


def check_close_shutdown(binary, root, threads, logs):
    with fixture(binary, root, threads, logs, timeout=2000) as server:
        client = start_task(server)
        server.event("ENTERED")
        client.socket.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, __import__('struct').pack('ii', 1, 0))
        client.close()
        for _ in range(16):
            with contextlib.closing(Client(server.port)) as replacement:
                replacement.send(request(headers=b"Connection: close\r\n"))
                replacement.response(200, b"home", True); replacement.eof()
        server.process.send_signal(signal.SIGTERM)
        server.event("NETWORK_STOPPED")
        require(server.process.poll() is None, "worker joined before gate release")
        server.process.send_signal(signal.SIGTERM)
        server.release()
    print("CASE A04 close/reuse/drain-before-join", threads, "PASS", flush=True)


def check_startup_cleanup(binary, root, logs):
    records = []
    for mode in ("before-port", "after-port", "exited-closed-stdin", "release-error"):
        seen = []
        original_error = TimeoutError("injected startup failure: " + mode)

        class FailingFixture(Fixture):
            def event(self, prefix):
                seen.append(self)
                if mode != "before-port":
                    super().event(prefix)
                if mode == "exited-closed-stdin":
                    self.process.kill()
                    self.process.wait(timeout=1)
                    self.process.stdin.close()
                raise original_error

            def release(self):
                if mode == "release-error":
                    raise BrokenPipeError("injected gate pipe failure")
                super().release()

        try:
            FailingFixture(binary, root, 0, logs)
            raise AssertionError("startup injection did not fail")
        except TimeoutError as error:
            require(error is original_error, "cleanup replaced original startup error")
        child = seen[0]
        record = {"mode": mode, "pid": child.process.pid,
                  "reaped": child.process.returncode is not None,
                  "reader_joined": not child.reader.is_alive(),
                  "pipes_closed": child.process.stdin.closed and child.process.stdout.closed,
                  "cleanup_log_exists": child.path.exists(), "outer_rescue": False}
        records.append(record)
        # Rescue is only for test failure containment, never credited as successful cleanup.
        if not record["reaped"] or not record["reader_joined"]:
            record["outer_rescue"] = True
            child.process.kill(); child.process.wait(timeout=1); child.reader.join(timeout=1)
        (logs / "startup-cleanup.json").write_text(json.dumps(records, indent=2))
        require(all(record[key] for key in ("reaped", "reader_joined", "pipes_closed", "cleanup_log_exists"))
                and not record["outer_rescue"], "startup cleanup required outer rescue")
    print("CASE A05 partial startup/PORT failure/exited child/pipe failure self-cleanup PASS", flush=True)


def main():
    binary, gate_binary = map(lambda p: str(Path(p).resolve()), sys.argv[1:3])
    parent = Path(os.environ["HP_S3_TEST_TMP_ROOT"]); parent.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="async-task-", dir=parent)); root = run / "www"; logs = run / "logs"
    root.mkdir(); logs.mkdir(); (root / "index.html").write_bytes(b"home")
    signal.signal(signal.SIGALRM, interrupt_suite); signal.alarm(120)
    passed = False
    try:
        check_startup_cleanup(gate_binary, root, logs)
        for threads in (0, 2):
            with running_server(binary, root, threads, logs, read_timeout=2000) as server:
                check_crud(server)
                exchange(server, expected={"tasks": ["wrong"]} if os.environ.get("HP_ASYNC_TASK_NEGATIVE") else {"tasks": []})
            metrics, _ = read_metrics(server)
            require(metrics["provider_errors_total"] == metrics["parse_errors_total"] == 0, "async attribution")
            check_responsive(gate_binary, root, threads, logs)
            check_timeouts(gate_binary, root, threads, logs)
            check_close_shutdown(gate_binary, root, threads, logs)
        check_capacity(gate_binary, root, logs)
        passed = True
    finally:
        signal.alarm(0); shutil.rmtree(root)
        (logs / "summary.json").write_text(json.dumps({"passed": passed, "fixtures_removed": not root.exists(), "children_reaped": True}))
        print("EVIDENCE", logs, "fixtures_removed", not root.exists(), flush=True)
    print("Async task HTTP PASS", flush=True)


if __name__ == "__main__":
    main()
