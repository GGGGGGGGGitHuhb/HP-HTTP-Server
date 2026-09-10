#!/usr/bin/env python3
"""Synthetic unit/fault probes, never used as measured benchmark results."""
import copy
import hashlib
import http.server
import importlib.util
import json
import os
import pathlib
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "benchmark"))
import run as bench


def summary():
    return {"schema": 1, "duration_us": 1000000, "requests": 100, "bytes": 104857600,
            "errors": dict.fromkeys(("connect", "read", "write", "status", "timeout"), 0),
            "latency_us": {"mean": 1000, "p50": 500, "p95": 2000, "p99": 3000, "max": 4000}}


def encoded(data):
    return bench.SUMMARY_PREFIX + json.dumps(data) + "\n"


class BenchTests(unittest.TestCase):
    def setUp(self):
        root = pathlib.Path(os.environ.get("HP_S3_TEST_TMP_ROOT", REPO / ".cache/benchmark-tests"))
        root.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="synthetic-", dir=root)
        self.root = pathlib.Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_parser_units(self):
        value = bench.parse_summary(encoded(summary()), 0, 1, 1048576)
        self.assertEqual(value["qps"], 100)
        self.assertEqual(value["received_mib_s"], 100)
        self.assertEqual(value["latency_ms"]["p99"], 3)

    def test_error_rejection(self):
        for key in summary()["errors"]:
            data = summary()
            data["errors"][key] = 1
            with self.subTest(key=key), self.assertRaisesRegex(bench.Invalid, "errors must be zero"):
                bench.parse_summary(encoded(data), 0, 1, 1024)

    def test_bad_summary_and_exit(self):
        with self.assertRaisesRegex(bench.Invalid, "nonzero"):
            bench.parse_summary(encoded(summary()), 7, 1, 1024)
        for text in ("Requests/sec: 999999999", "BENCH_SUMMARY {broken", encoded(summary()) * 2):
            with self.assertRaises(bench.Invalid):
                bench.parse_summary(text, 0, 1, 1024)
        for mutate in (lambda d: d.pop("requests"), lambda d: d["errors"].pop("timeout"), lambda d: d.update(requests=0), lambda d: d.update(bytes=1), lambda d: d["latency_us"].update(p99=float("nan")), lambda d: d.update(duration_us=1)):
            data = summary()
            mutate(data)
            with self.assertRaises(bench.Invalid):
                bench.parse_summary(encoded(data), 0, 1, 1024)

    def test_fixture_schedule_aggregate(self):
        payload = bench.fixture(self.root, 1024)
        self.assertEqual(payload["sha256"], hashlib.sha256(bytes(range(256)) * 4).hexdigest())
        self.assertEqual(bench.schedule(), [(1,1024,"A"),(1,1024,"B"),(1,1048576,"A"),(1,1048576,"B"),(2,1048576,"B"),(2,1048576,"A"),(2,1024,"B"),(2,1024,"A"),(3,1024,"A"),(3,1024,"B"),(3,1048576,"A"),(3,1048576,"B")])
        rows = []
        for size in (1024,1048576):
            for label in ("A","B"):
                for value in (80,100,120):
                    rows.append({"status":"valid","label":label,"payload":{"size":size},"measurement":{"qps":value*(2 if label=="B" else 1),"latency_ms":{"p99":3}}})
        result=bench.aggregate(rows)
        self.assertEqual(result["B_over_A_qps_median"]["1024"],2)
        self.assertEqual(result["groups"]["A-1024"]["qps_relative_span"],.4)
        self.assertTrue(result["groups"]["A-1024"]["noisy"])
        rows[0]["status"]="invalid"
        with self.assertRaisesRegex(bench.Invalid,"incomplete or invalid"):bench.aggregate(rows)

    def test_manifest_rejection(self):
        path=self.root/"false-manifest.json"
        path.write_text(json.dumps({"label":"A","commit":"false","tree":bench.TREES["A"]}))
        with self.assertRaisesRegex(bench.Invalid,"commit/tree mismatch"):bench.validate_manifest(path,"A")
        binary=self.root/"binary";binary.write_bytes(b"original")
        manifest={"binary":str(binary),"binary_sha256":bench.sha(binary)}
        payload=bench.fixture(self.root,1024)
        binary.write_bytes(b"changed")
        with self.assertRaisesRegex(bench.Invalid,"binary changed"):bench.invariant(manifest,self.root,payload)

    def test_tool_missing_wrong(self):
        with self.assertRaisesRegex(bench.Invalid,"missing"):bench.validate_tool(self.root/"absent")
        fake=self.script("wrong","print('wrk wrong version')")
        with self.assertRaisesRegex(bench.Invalid,"wrong version or binary hash"):bench.validate_tool(fake)

    def test_binary_rejection(self):
        binary = self.root / "changed-binary"
        binary.write_bytes(b"original")
        manifest = {"binary": str(binary), "binary_sha256": bench.sha(binary)}
        payload = bench.fixture(self.root, 1024)
        binary.write_bytes(b"changed")
        with self.assertRaisesRegex(bench.Invalid, "binary changed"):
            bench.invariant(manifest, self.root, payload)

    def test_whole_run_failure_status(self):
        # Synthetic injection at the sample boundary verifies main never
        # publishes a partial successful aggregate, including interruption.
        manifest = {"compiler": "synthetic", "cmake": "synthetic"}
        for index, error in enumerate((bench.Invalid("server startup failed"),
                bench.Invalid("wrk nonzero exit"), bench.Invalid("audit body"),
                bench.Invalid("server exited during wrk"), KeyboardInterrupt())):
            output = self.root / f"invalid-{index}"
            with mock.patch.object(bench, "validate_manifest", return_value=manifest), \
                    mock.patch.object(bench, "validate_tool", return_value={}), \
                    mock.patch.object(bench, "run_sample", side_effect=error):
                code = bench.main(["--baseline-manifest", "synthetic", "--candidate-manifest", "synthetic",
                                   "--wrk", "synthetic", "--output", str(output)])
            self.assertEqual(code, 1)
            result = json.loads((output / "run.json").read_text())
            self.assertEqual(result["status"], "invalid")
            self.assertEqual(result["log_limit_bytes"], 2147483648)
            self.assertEqual(result["min_free_disk_bytes"], 4294967296)
            self.assertEqual(result["observed_log_bytes_at_stop"], 0)
            self.assertNotIn("summary", result)
            self.assertFalse((output / "root").exists())

    def test_directory_resources_and_log(self):
        sentinel=self.root/"sentinel";sentinel.write_text("keep")
        with self.assertRaisesRegex(bench.Invalid,"not empty"):
            bench.main(["--baseline-manifest","no","--candidate-manifest","no","--wrk","no","--output",str(self.root)])
        self.assertEqual(sentinel.read_text(),"keep")
        with mock.patch.object(bench.shutil,"disk_usage",return_value=type("Disk",(),{"free":0})()):
            with self.assertRaisesRegex(bench.Invalid,"disk free"):bench.resources_ok(self.root)
        with mock.patch.object(bench.resource,"getrlimit",return_value=(10,10)):
            with self.assertRaisesRegex(bench.Invalid,"nofile"):bench.resources_ok(self.root)
        log=self.root/"oversize.stdout"
        with log.open("wb") as stream:stream.truncate(bench.LOG_LIMIT+1)
        with self.assertRaisesRegex(bench.Invalid,"log limit"):bench.log_guard(self.root)

    def http_server(self, bad_body=False, status=200):
        payload=bench.fixture(self.root,1024)
        body=(self.root/payload["name"]).read_bytes()
        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version="HTTP/1.1"
            def do_GET(self):
                self.send_response(status);self.send_header("Content-Length",str(len(body)));self.end_headers()
                self.wfile.write(bytes(len(body)) if bad_body else body)
            def log_message(self,*args):pass
        server=http.server.ThreadingHTTPServer(("127.0.0.1",0),Handler)
        thread=threading.Thread(target=server.serve_forever);thread.start()
        self.addCleanup(server.server_close)
        self.addCleanup(thread.join)
        self.addCleanup(server.shutdown)
        return server.server_port,payload

    def test_body_rejection(self):
        port,payload=self.http_server(bad_body=True)
        with self.assertRaisesRegex(bench.Invalid,"body length/hash mismatch"):bench.audit(port,payload)

    def test_status_rejection(self):
        port,payload=self.http_server(status=302)
        with self.assertRaisesRegex(bench.Invalid,"status must"):bench.audit(port,payload)

    def test_keepalive_audit(self):
        port,payload=self.http_server()
        self.assertEqual(len(bench.audit(port,payload)),5)

    def raw_audit_server(self, tail=b"", truncate=False, close_after=False):
        payload = bench.fixture(self.root, 1024)
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0)); listener.listen(); listener.settimeout(3)
        port = listener.getsockname()[1]
        sends = []
        errors = []
        def serve():
            try:
                connection, _ = listener.accept()
                with connection:
                    connection.settimeout(3)
                    while True:
                        request = b""
                        while b"\r\n\r\n" not in request:
                            chunk = connection.recv(4096)
                            if not chunk:
                                return
                            request += chunk
                        body = (bytes(range(256)) * 4)[:512 if truncate else 1024]
                        # One sendall deliberately places the valid body and
                        # unexpected bytes in the same small response write.
                        packet = b"HTTP/1.1 200 OK\r\nContent-Length: 1024\r\nConnection: keep-alive\r\n\r\n" + body + tail
                        connection.sendall(packet)
                        sends.append(len(packet))
                        if truncate or close_after:
                            return
            except Exception as error:
                errors.append(str(error))
            finally:
                listener.close()
        thread = threading.Thread(target=serve)
        thread.start()
        def finish():
            thread.join(4)
            self.assertFalse(thread.is_alive())
            self.assertEqual(errors, [])
        self.addCleanup(finish)
        return port, payload, sends

    def test_same_write_keepalive_five(self):
        port, payload, sends = self.raw_audit_server()
        self.assertEqual(len(bench.audit(port, payload)), 5)
        self.assertEqual(len(sends), 5)

    def test_same_write_tail_rejection(self):
        port, payload, _ = self.raw_audit_server(tail=b"UNEXPECTED-TAIL")
        with self.assertRaisesRegex(bench.Invalid, "unexpected trailing bytes"):
            bench.audit(port, payload)

    def test_premature_eof_rejection(self):
        port, payload, _ = self.raw_audit_server(truncate=True)
        with self.assertRaisesRegex(bench.Invalid, "premature EOF in body"):
            bench.audit(port, payload)

    def test_keepalive_eof_rejection(self):
        port, payload, _ = self.raw_audit_server(close_after=True)
        with self.assertRaisesRegex(bench.Invalid, "trailing bytes or EOF"):
            bench.audit(port, payload)

    def test_tail_whole_run_real_process(self):
        server = self.script("tail-server", """import signal,socket,sys
signal.signal(signal.SIGTERM,lambda *_:sys.exit(0))
s=socket.socket();s.bind(('127.0.0.1',0));s.listen()
print('listening on port '+str(s.getsockname()[1])+'.',flush=True)
while True:
 c,_=s.accept()
 with c:
  request=b''
  while b'\\r\\n\\r\\n' not in request:
   chunk=c.recv(4096)
   if not chunk:break
   request+=chunk
  c.sendall(b'HTTP/1.1 200 OK\\r\\nContent-Length: 1024\\r\\nConnection: keep-alive\\r\\n\\r\\n'+bytes(range(256))*4+b'UNEXPECTED-TAIL')
  print('injected_tail_bytes=14 one_sendall',flush=True)
""")
        output = self.root / "tail-run"
        coordinator = self.script("tail-coordinator", f"""import sys,pathlib
sys.path.insert(0,{str(REPO / 'benchmark')!r})
import run as bench
manifest={{'label':'B','commit':'synthetic-tail-server','compiler':'synthetic','cmake':'synthetic','binary':{str(server)!r},'binary_sha256':bench.sha({str(server)!r})}}
bench.validate_manifest=lambda path,label:dict(manifest,label=label)
bench.validate_tool=lambda path:{{'synthetic':True}}
raise SystemExit(bench.main(['--baseline-manifest','synthetic','--candidate-manifest','synthetic','--wrk','unused','--output',{str(output)!r},'--smoke']))
""")
        with (self.root / "tail-coordinator.log").open("w") as log:
            result = subprocess.run([str(coordinator)], stdout=log, stderr=log, timeout=12)
        self.assertEqual(result.returncode, 1)
        detail = json.loads((output / "run.json").read_text())
        self.assertEqual(detail["status"], "invalid")
        self.assertIn("unexpected trailing bytes", detail["error"])
        self.assertNotIn("summary", detail)
        sample = next(output.glob("sample-*/sample.json"))
        row = json.loads(sample.read_text())
        self.assertEqual(row["status"], "invalid")
        self.assertTrue(row["cleanup"]["reaped"])
        self.assertEqual(row["cleanup"]["returncode"], 0)
        self.assertFalse(pathlib.Path(f"/proc/{row['server_identity']['pid']}").exists())
        connection = socket.socket(); connection.settimeout(.3)
        self.assertNotEqual(connection.connect_ex(("127.0.0.1", row["port"])), 0)
        connection.close()
        self.assertIn("injected_tail_bytes=14", (sample.parent / "server.stdout").read_text())
        self.assertFalse((output / "root").exists())

    def script(self,name,source):
        path=self.root/name
        path.write_text("#!/usr/bin/env python3\n"+source+"\n")
        path.chmod(0o755)
        return path

    def sleeper(self,name="sleeper"):
        return self.script(name,"import time\ntime.sleep(60)")

    def reap_fallback(self,owned):
        if owned.process.poll() is None:
            owned.process.kill();owned.process.wait(timeout=3)
        owned.stdout.close();owned.stderr.close()

    def test_cleanup(self):
        worker=bench.OwnedProcess([str(self.sleeper())],self.root/"owned")
        try:
            worker.close()
            self.assertIsNotNone(worker.process.poll(),"owned child remains alive after cleanup")
        finally:self.reap_fallback(worker)

    def test_startup_failure(self):
        child=bench.OwnedProcess([str(self.script("bad-server","raise SystemExit(3)"))],self.root/"start")
        try:
            with self.assertRaisesRegex(bench.Invalid,"startup failed"):bench.ready(child,self.root,time.monotonic()+2)
        finally:self.reap_fallback(child)

    def run_fake_wrk(self,source,expected,deadline_seconds=3,kill_server=False):
        server=bench.OwnedProcess([str(self.sleeper("server"))],self.root/"server-process")
        tool=self.script("synthetic-wrk",source)
        payload=bench.fixture(self.root,1024)
        killer=None
        if kill_server:
            killer=threading.Timer(.15,lambda:server.process.terminate());killer.start()
        try:
            with self.assertRaisesRegex(bench.Invalid,expected):
                bench.wrk_run(tool,server,1,payload,1,self.root/"wrk",self.root,time.monotonic()+deadline_seconds)
            cleanup=json.loads((self.root/"wrk.cleanup.json").read_text())
            self.assertTrue(cleanup["reaped"])
        finally:
            if killer:killer.join()
            self.reap_fallback(server)

    def test_wrk_nonzero_real_process(self):
        self.run_fake_wrk("print("+repr(encoded(summary()))+")\nraise SystemExit(7)","nonzero exit")

    def test_wrk_malformed_real_process(self):
        self.run_fake_wrk("print('BENCH_SUMMARY bad')","malformed")

    def test_server_exit_real_process(self):
        self.run_fake_wrk("import time\ntime.sleep(60)","server exited",kill_server=True)

    def test_watchdog_real_process(self):
        self.run_fake_wrk("import time\ntime.sleep(60)","watchdog",deadline_seconds=.15)

    def test_log_limit_real_process(self):
        # Isolated coordinator only: production CLI retains its fixed 2GiB.
        coordinator = self.script("small-budget-coordinator", f'''import sys,pathlib,time,json
sys.path.insert(0,{str(REPO / "benchmark")!r})
import run as bench
out=pathlib.Path({str(self.root)!r})
original=bench.log_guard
bench.log_guard=lambda root: original(root,65536)
server=bench.OwnedProcess([sys.executable,"-c","import time;time.sleep(60)"],out/"budget-server")
tool=out/"budget-wrk"
tool.write_text("#!/usr/bin/env python3\\nimport sys,time\\nsys.stdout.buffer.write(b'x'*131072);sys.stdout.flush();time.sleep(60)\\n")
tool.chmod(0o755)
result={{"status":"invalid","test_only_log_limit":65536,"formal_log_limit":bench.LOG_LIMIT}}
try:
    bench.wrk_run(tool,server,1,bench.fixture(out,1024),1,out/"budget-client",out,time.monotonic()+3)
    result["status"]="unexpected-success"
except bench.Invalid as error:
    result["error"]=str(error)
finally:
    result["server_cleanup"]=server.close()
    result["observed_log_bytes"]=bench.log_bytes(out)
    bench.save(out/"budget-result.json",result)
raise SystemExit(1 if result["status"]=="invalid" else 0)
''')
        with (self.root / "coordinator.log").open("w") as log:
            result = subprocess.run([str(coordinator)], stdout=log, stderr=log, timeout=10)
        self.assertEqual(result.returncode, 1)
        detail = json.loads((self.root / "budget-result.json").read_text())
        self.assertEqual(detail["status"], "invalid")
        self.assertEqual(detail["error"], "log limit exceeded")
        self.assertEqual(detail["formal_log_limit"], 2147483648)
        self.assertGreater(detail["observed_log_bytes"], 65536)
        self.assertTrue((self.root / "budget-client.stdout").exists())
        for name in ("budget-client", "budget-server"):
            identity = json.loads((self.root / (name + ".process.json")).read_text())
            self.assertFalse(pathlib.Path(f"/proc/{identity['pid']}").exists())
        self.assertTrue(detail["server_cleanup"]["reaped"])
        self.assertTrue(json.loads((self.root / "budget-client.cleanup.json").read_text())["reaped"])

    def test_budget_boundaries(self):
        self.assertEqual(bench.LOG_LIMIT, 2147483648)
        self.assertEqual(bench.MIN_FREE_DISK, 4294967296)
        first = self.root / "sample-01"
        second = self.root / "sample-02"
        first.mkdir(); second.mkdir()
        part = first / "server.stderr"
        with part.open("wb") as stream:
            stream.truncate(bench.LOG_LIMIT // 2)
        other = second / "measurement.stdout"
        for offset in (-1, 0, 1):
            with other.open("wb") as stream:
                stream.truncate(bench.LOG_LIMIT // 2 + offset)
            self.assertEqual(bench.log_bytes(self.root), bench.LOG_LIMIT + offset)
            if offset <= 0:
                self.assertEqual(bench.log_guard(self.root), bench.LOG_LIMIT + offset)
            else:
                with self.assertRaisesRegex(bench.Invalid, "log limit"):
                    bench.log_guard(self.root)
        # Sparse logical-length checks, explicitly not 2GiB of actual writes.
        self.assertLess(part.stat().st_blocks * 512 + other.stat().st_blocks * 512, 1048576)
        for offset in (-1, 0):
            disk = type("Disk", (), {"free": bench.MIN_FREE_DISK + offset})()
            with mock.patch.object(bench.shutil, "disk_usage", return_value=disk):
                if offset < 0:
                    with self.assertRaisesRegex(bench.Invalid, "4GiB disk free"):
                        bench.resources_ok(self.root)
                else:
                    self.assertEqual(bench.resources_ok(self.root)["disk_free_bytes"], bench.MIN_FREE_DISK)

    def test_external_interrupt_real_process(self):
        # The harness sends SIGINT to this owned coordinator only; it cleans its
        # own long-lived child and saves proof even while unwinding KeyboardInterrupt.
        script=self.script("interrupt-coordinator",f'''import sys,pathlib,time,json
sys.path.insert(0,{str(REPO / "benchmark")!r})
import run as bench
out=pathlib.Path({str(self.root)!r})
child=bench.OwnedProcess([sys.executable,"-c","import time;time.sleep(60)"],out/"interrupt-child")
(out/"ready").write_text(str(child.process.pid))
try:
    time.sleep(60)
except KeyboardInterrupt:
    (out/"interrupted").write_text("hit")
finally:
    bench.save(out/"interrupt-cleanup.json",child.close())
raise SystemExit(1)
''')
        parent=subprocess.Popen([str(script)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        child_pid=None
        try:
            deadline=time.monotonic()+3
            while not (self.root/"ready").exists() and time.monotonic()<deadline:time.sleep(.01)
            self.assertTrue((self.root/"ready").exists())
            child_pid=int((self.root/"ready").read_text())
            parent.send_signal(signal.SIGINT)
            self.assertEqual(parent.wait(timeout=5),1)
            self.assertEqual((self.root/"interrupted").read_text(),"hit")
            self.assertTrue(json.loads((self.root/"interrupt-cleanup.json").read_text())["reaped"])
            self.assertFalse(pathlib.Path(f"/proc/{child_pid}").exists())
        finally:
            if parent.poll() is None:parent.kill();parent.wait()
            if child_pid and pathlib.Path(f"/proc/{child_pid}").exists():os.kill(child_pid,signal.SIGKILL)


if __name__ == "__main__":
    unittest.main(verbosity=2)
