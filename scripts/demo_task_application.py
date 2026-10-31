#!/usr/bin/env python3
"""使用临时合成数据演示任务 API；只依赖 Python 标准库。"""
import argparse
import contextlib
import http.client
import json
import os
from pathlib import Path
import queue
import re
import shutil
import signal
import subprocess
import tempfile
import threading
import time

MAX_LOG = 16 * 1024 * 1024
LISTEN_NOTICE = ("监听说明：服务器按现有行为监听 0.0.0.0 的临时端口；客户端连接 127.0.0.1。"
                 "网络可达的其他主机可能读取临时文件或读写无鉴权任务 API，随机端口不是访问控制。"
                 "请仅在确认可信的开发环境运行。")


def require(condition, message):
    if not condition:
        raise AssertionError(message)


class DemoServer:
    """拥有一个生产进程、输出 reader 及管道；root 由调用者拥有。"""
    def __init__(self, binary, root, logs, threads=0):
        self.process = None
        self.reader = None
        self.port = None
        self.port_queue = queue.Queue()
        self.reader_error = None
        self.forced = False
        self.cleaned = False
        self.cleanup_errors = []
        self.original_error = None
        self.log_path = Path(logs) / f"server-{time.monotonic_ns()}.log"
        self.command = [str(binary), "--port", "0", "--root", str(root), "--threads", str(threads),
                        "--idle-timeout-ms", "5000", "--keep-alive-timeout-ms", "5000"]
        print(LISTEN_NOTICE, flush=True)
        try:
            self.process = subprocess.Popen(self.command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.reader = threading.Thread(target=self.collect_output, name="demo-server-output")
            self.reader.start()
            self.port = self.wait_port()
        except BaseException as error:
            self.original_error = repr(error)
            self.cleanup()
            if self.cleanup_errors:
                error.add_note("cleanup: " + "; ".join(self.cleanup_errors))
            raise

    def wait_port(self):
        return self.port_queue.get(timeout=5)

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
                        raise RuntimeError("server log exceeds 16 MiB")
                    output.write(block)
                    size += len(block)
                    pending = (pending + block)[-8192:]
                    match = re.search(rb"listening on port ([0-9]+)\.", pending)
                    if match and self.port_queue.empty():
                        self.port_queue.put(int(match.group(1)))
        except BaseException as error:
            self.reader_error = repr(error)
            if self.process.poll() is None:
                self.process.kill()

    def cleanup(self):
        if self.cleaned:
            return self.record

        def attempt(action):
            try:
                action()
            except BaseException as error:
                self.cleanup_errors.append(repr(error))

        process = self.process
        if process is not None:
            if process.poll() is None:
                attempt(lambda: process.send_signal(signal.SIGTERM))
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.forced = True
                attempt(process.kill)
                attempt(lambda: process.wait(timeout=5))
            except BaseException as error:
                self.cleanup_errors.append(repr(error))
                attempt(process.kill)
                attempt(lambda: process.wait(timeout=5))
        if self.reader is not None and self.reader.ident is not None:
            attempt(lambda: self.reader.join(timeout=5))
        if process is not None and process.stdout is not None:
            attempt(process.stdout.close)
        self.record = {"argv": self.command, "pid": process.pid if process else None,
                       "returncode": process.returncode if process else None,
                       "reaped": process is None or process.returncode is not None,
                       "reader_joined": self.reader is None or not self.reader.is_alive(),
                       "pipes_closed": process is None or process.stdout is None or process.stdout.closed,
                       "forced": self.forced, "reader_error": self.reader_error,
                       "cleanup_errors": self.cleanup_errors, "original_error": self.original_error,
                       "listen_address": "0.0.0.0", "client_address": "127.0.0.1", "port": self.port}
        attempt(lambda: self.log_path.with_suffix(".json").write_text(json.dumps(self.record, indent=2)))
        self.cleaned = True
        return self.record

    def __enter__(self):
        return self

    def __exit__(self, error_type, error, traceback):
        if error is not None:
            self.original_error = repr(error)
        record = self.cleanup()
        clean = (record["reaped"] and record["reader_joined"] and record["pipes_closed"] and
                 not record["cleanup_errors"] and record["reader_error"] is None)
        if error is not None:
            if not clean:
                error.add_note("server cleanup incomplete: " + repr(record))
        else:
            require(clean and record["returncode"] == 0 and not record["forced"],
                    "server cleanup failed: " + repr(record))
        return False


def exchange(server, method, path, body=None, expected_status=200):
    encoded = None if body is None else json.dumps(body, ensure_ascii=False).encode("utf-8")
    headers = {} if body is None else {"Content-Type": "application/json"}
    with contextlib.closing(http.client.HTTPConnection("127.0.0.1", server.port, timeout=5)) as client:
        client.request(method, path, body=encoded, headers=headers)
        response = client.getresponse()
        payload = response.read(262145)
        require(response.status == expected_status, f"{method} {path}: {response.status}")
        require(int(response.getheader("Content-Length")) == len(payload), "response framing")
        if path.startswith("/api/tasks"):
            require(response.getheader("Cache-Control") == "no-store", "task no-store")
            require(response.getheader("Content-Type") == "application/json", "task media")
            require(len(payload) <= 262144, "task response budget")
            return json.loads(payload)
        return payload


def finalize_output(output, root, summary, original_error=None):
    """清理失败保留原始异常；证据写入错误不能覆盖业务/测试失败。"""
    errors = []
    if root is not None:
        try:
            shutil.rmtree(root)
        except BaseException as error:
            errors.append("root cleanup: " + repr(error))
    summary["root_removed"] = root is None or not root.exists()
    summary["cleanup_errors"] = errors
    if errors:
        summary["passed"] = False
    try:
        (output / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2))
    except BaseException as error:
        errors.append("summary write: " + repr(error))
    if errors:
        if original_error is not None:
            original_error.add_note("; ".join(errors))
        else:
            raise RuntimeError("; ".join(errors))


def demonstrate(binary, output):
    binary = Path(binary).resolve(strict=True)
    output = Path(output).absolute()
    output.mkdir(parents=False, exist_ok=False)
    root = None
    records = []
    steps = []
    original_error = None
    summary = {"passed": False, "listen_address": "0.0.0.0", "client_address": "127.0.0.1"}
    try:
        root = Path(tempfile.mkdtemp(prefix="static-", dir=output))
        (root / "index.html").write_bytes(b"temporary task demo\n")
        with DemoServer(binary, root, output) as first:
            records.append(first)
            require(exchange(first, "GET", "/") == b"temporary task demo\n", "static bytes")
            require(exchange(first, "POST", "/api/echo", {"echo": "示例"}) ==
                    json.dumps({"echo": "示例"}, ensure_ascii=False).encode(), "echo bytes")
            created = exchange(first, "POST", "/api/tasks", {"title": "合成任务", "completed": False}, 201)
            identifier = created["id"]
            require(isinstance(identifier, str) and identifier.isdecimal(), "dynamic identifier")
            path = "/api/tasks/" + identifier
            require(exchange(first, "GET", path) == created, "created record")
            changed = exchange(first, "PUT", path, {"title": "已替换", "completed": True})
            require(changed == {"id": identifier, "title": "已替换", "completed": True}, "full replacement")
            require(exchange(first, "GET", "/api/tasks") == {"tasks": [changed]}, "list snapshot")
            require(exchange(first, "DELETE", path) == {"id": identifier, "deleted": True}, "delete record")
            missing = exchange(first, "GET", path, expected_status=404)
            require(missing["error"]["code"] == "task_not_found", "deleted lookup")
            survivor = exchange(first, "POST", "/api/tasks", {"title": "重启会清空", "completed": False}, 201)
            require(exchange(first, "GET", "/api/tasks") == {"tasks": [survivor]}, "before restart")
            steps.append({"step": "static/echo/create/get/put/list/delete", "id": identifier, "port": first.port})
            print(f"静态文件、echo 和任务 CRUD 通过，动态 ID={identifier}", flush=True)
        with DemoServer(binary, root, output) as second:
            records.append(second)
            require(exchange(second, "GET", "/api/tasks") == {"tasks": []}, "restart must clear memory")
            steps.append({"step": "restart clears memory", "port": second.port})
            print("重启后任务列表为空：数据仅驻留内存。", flush=True)
        summary["passed"] = True
    except BaseException as error:
        original_error = error
        summary["original_error"] = repr(error)
        raise
    finally:
        summary.update(steps=steps, servers=[server.record for server in records])
        finalize_output(output, root, summary, original_error)
    return summary


def interrupted(signum, frame):
    raise RuntimeError(f"demo interrupted by signal {signum}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path, help="父目录须存在；输出目录不得已存在")
    options = parser.parse_args()
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    demonstrate(options.server, options.output)
    print(f"演示通过，证据保存在 {options.output}", flush=True)


if __name__ == "__main__":
    main()
