#!/usr/bin/env python3
"""Fixed localhost wrk comparison; raw evidence is kept on every outcome."""
import argparse
import hashlib
import io
import json
import math
import os
import pathlib
import re
import resource
import select
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import tarfile
import time
from datetime import datetime, timezone

from build import COMMITS, TREES, FLAGS, archive_bytes, sha

HERE = pathlib.Path(__file__).resolve().parent
WRK_SHA256 = "b10e53769443c2bf3be2cdedec8ef6571aa5bfd1494796b247f3f9296e3af71d"
SUMMARY_PREFIX = "BENCH_SUMMARY "
LOG_LIMIT = 2 * 1024 * 1024 * 1024
MIN_FREE_DISK = 4 * 1024 * 1024 * 1024
SERVER_ARGS = ["--threads", "2", "--idle-timeout-ms", "30000", "--keep-alive-timeout-ms", "15000", "--shutdown-timeout-ms", "5000", "--port", "0"]


class Invalid(RuntimeError):
    pass


def demand(condition, message):
    if not condition:
        raise Invalid(message)


def utc():
    return datetime.now(timezone.utc).isoformat()


def save(path, data):
    pathlib.Path(path).write_text(json.dumps(data, indent=2, allow_nan=False) + "\n")


def validate_manifest(path, label):
    data = json.loads(pathlib.Path(path).read_text())
    demand(data.get("label") == label and data.get("commit") == COMMITS[label] and data.get("tree") == TREES[label], "manifest fixed commit/tree mismatch")
    expected_archive = archive_bytes(label)
    expected_hash = hashlib.sha256(expected_archive).hexdigest()
    demand(data.get("archive_sha256") == expected_hash and sha(data["archive"]) == expected_hash, "manifest archive mismatch")
    with tarfile.open(fileobj=io.BytesIO(expected_archive)) as tf:
        expected_sources = {member.name: hashlib.sha256(tf.extractfile(member).read()).hexdigest() for member in tf if member.isfile()}
    demand(data.get("source_hashes") == expected_sources, "manifest source map mismatch")
    for name, digest in expected_sources.items():
        demand(sha(pathlib.Path(data["source"]) / name) == digest, "exported source changed")
    demand(data.get("flags") == FLAGS, "Release build flags mismatch")
    for name in ("binary", "cmake_cache", "compile_commands"):
        demand(sha(data[name]) == data.get(name + "_sha256"), "manifest " + name + " hash mismatch")
    commands = json.loads(pathlib.Path(data["compile_commands"]).read_text())
    demand(bool(commands), "empty compile commands")
    for item in commands:
        command = item["command"]
        demand(all(flag in command for flag in ("-O3", "-DNDEBUG", "-std=c++20")), "compiled flags mismatch")
        demand(not any(flag in command for flag in ("-fsanitize", "-flto", "-march", "-mtune")), "noncomparable compiled flags")
    return data


def validate_tool(path):
    path = pathlib.Path(path).resolve()
    demand(path.is_file() and os.access(path, os.X_OK), "wrk missing or not executable")
    demand(sha(path) == WRK_SHA256, "wrk wrong version or binary hash")
    version = subprocess.run([str(path), "--version"], capture_output=True, text=True, timeout=5)
    # This official package prints version + usage and exits 1 without a URL.
    demand("wrk debian/4.1.0-4build2" in version.stdout + version.stderr, "wrk version not runnable")
    dependencies = subprocess.run(["ldd", str(path)], capture_output=True, text=True, check=True).stdout
    demand("not found" not in dependencies, "wrk dependency missing")
    libraries = {}
    for line in dependencies.splitlines():
        match = re.search(r"(?:=>\s+)?(/\S+)\s+\(", line)
        if match:
            libraries[match.group(1)] = sha(match.group(1))
    return {"binary": str(path), "sha256": sha(path), "version": (version.stdout + version.stderr).splitlines()[0], "version_exit": version.returncode, "libraries": libraries, "ldd": dependencies}


def parse_summary(text, returncode, seconds, body_size):
    demand(returncode == 0, "wrk nonzero exit")
    lines = [line[len(SUMMARY_PREFIX):] for line in text.splitlines() if line.startswith(SUMMARY_PREFIX)]
    demand(len(lines) == 1, "missing or duplicate wrk summary")
    try:
        data = json.loads(lines[0])
        demand(data["schema"] == 1, "summary schema")
        for name in ("duration_us", "requests", "bytes"):
            value = data[name]
            demand(type(value) in (int, float) and math.isfinite(value) and value >= 0 and int(value) == value, "bad summary count")
        for name in ("connect", "read", "write", "status", "timeout"):
            value = data["errors"][name]
            demand(type(value) in (int, float) and math.isfinite(value) and value == 0, "wrk errors must be zero")
        demand(data["requests"] > 0 and seconds * .8e6 <= data["duration_us"] <= (seconds + 3) * 1e6, "wrk requests or duration invalid")
        demand(data["bytes"] >= data["requests"] * body_size, "wrk received fewer than audited payload bytes")
        latency = data["latency_us"]
        for name in ("mean", "p50", "p95", "p99", "max"):
            demand(type(latency[name]) in (int, float) and math.isfinite(latency[name]) and latency[name] >= 0, "bad latency value")
        demand(latency["p50"] <= latency["p95"] <= latency["p99"] <= latency["max"] and latency["mean"] <= latency["max"], "latency order invalid")
    except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        raise Invalid("malformed wrk summary") from error
    elapsed = data["duration_us"] / 1e6
    return {**data, "elapsed_seconds": elapsed, "qps": data["requests"] / elapsed, "received_mib_s": data["bytes"] / elapsed / 1048576, "latency_ms": {k: v / 1000 for k, v in latency.items()}}


def fixture(root, size):
    path = pathlib.Path(root) / f"payload-{size}.bin"
    path.write_bytes((bytes(range(256)) * ((size + 255) // 256))[:size])
    return {"name": path.name, "size": size, "sha256": sha(path)}


def audit(port, payload, count=5):
    rows = []
    # Own every received byte. HTTPResponse's buffered file can read beyond
    # Content-Length and discard a tail when that response is closed.
    pending = bytearray()
    with socket.create_connection(("127.0.0.1", port), timeout=3) as connection:
        for _ in range(count):
            request = (f"GET /{payload['name']} HTTP/1.1\r\n"
                       f"Host: 127.0.0.1:{port}\r\nConnection: keep-alive\r\n\r\n")
            connection.sendall(request.encode("ascii"))
            while b"\r\n\r\n" not in pending:
                chunk = connection.recv(65536)
                demand(bool(chunk), "audit premature EOF in headers")
                pending.extend(chunk)
                demand(pending.find(b"\r\n\r\n") < 65536 and len(pending) <= 131072,
                       "audit response headers too large")
            header_end = pending.index(b"\r\n\r\n") + 4
            try:
                lines = bytes(pending[:header_end - 4]).decode("ascii").split("\r\n")
                status = lines[0].split(" ", 2)
                demand(len(status) >= 2 and status[:2] == ["HTTP/1.1", "200"],
                       "audit status must be HTTP/1.1 200")
                headers = {}
                for line in lines[1:]:
                    name, separator, value = line.partition(":")
                    demand(bool(separator) and bool(name) and name == name.strip(), "audit malformed header")
                    headers.setdefault(name.lower(), []).append(value.strip())
            except UnicodeDecodeError as error:
                raise Invalid("audit non-ASCII headers") from error
            demand(headers.get("content-length") == [str(payload["size"])] and "transfer-encoding" not in headers,
                   "audit Content-Length mismatch")
            connection_tokens = ",".join(headers.get("connection", [])).lower().split(",")
            demand("close" not in [token.strip() for token in connection_tokens], "audit unexpected connection close")
            response_end = header_end + payload["size"]
            while len(pending) < response_end:
                chunk = connection.recv(65536)
                demand(bool(chunk), "audit premature EOF in body")
                pending.extend(chunk)
            demand(len(pending) == response_end, "audit unexpected trailing bytes")
            body = bytes(pending[header_end:response_end])
            digest = hashlib.sha256(body).hexdigest()
            demand(len(body) == payload["size"] and digest == payload["sha256"], "audit body length/hash mismatch")
            del pending[:response_end]
            if select.select([connection], [], [], .01)[0]:
                extra = connection.recv(65536)
                pending.extend(extra)
                demand(False, "audit unexpected trailing bytes or EOF")
            rows.append({"status": 200, "bytes": len(body), "sha256": digest})
    return rows


def process_info(pid):
    base = pathlib.Path("/proc") / str(pid)
    fields = (base / "stat").read_text().rsplit(")", 1)[1].split()
    status = dict(line.split(":", 1) for line in (base / "status").read_text().splitlines() if ":" in line)
    return {"pid": pid, "starttime": int(fields[19]), "cpu_seconds": (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK"), "rss_kib": int(status["VmRSS"].split()[0]) if "VmRSS" in status else None, "hwm_kib": int(status["VmHWM"].split()[0]) if "VmHWM" in status else None}


def listener_owned(pid, port):
    inodes = set()
    for fd in (pathlib.Path("/proc") / str(pid) / "fd").iterdir():
        try:
            target = os.readlink(fd)
        except FileNotFoundError:
            continue
        if target.startswith("socket:["):
            inodes.add(target[8:-1])
    for line in pathlib.Path("/proc/net/tcp").read_text().splitlines()[1:]:
        fields = line.split()
        if int(fields[1].split(":")[1], 16) == port and fields[3] == "0A" and fields[9] in inodes:
            return True
    return False


class OwnedProcess:
    def __init__(self, command, prefix):
        self.command = command
        self.stdout_path, self.stderr_path = pathlib.Path(str(prefix) + ".stdout"), pathlib.Path(str(prefix) + ".stderr")
        self.stdout = self.stdout_path.open("wb")
        self.stderr = self.stderr_path.open("wb")
        try:
            self.process = subprocess.Popen(command, stdout=self.stdout, stderr=self.stderr)
            self.identity = process_info(self.process.pid)
        except BaseException:
            if hasattr(self, "process"):
                # Popen retains this unreaped child's identity even if /proc
                # acquisition fails immediately after exec.
                self.process.kill()
                self.process.wait(timeout=3)
            self.stdout.close()
            self.stderr.close()
            raise
        self.forced = False
        self.stop_result = None
        save(str(prefix) + ".process.json", {"command": command, **self.identity})

    def alive(self):
        return self.process.poll() is None

    def matches(self):
        try:
            return process_info(self.process.pid)["starttime"] == self.identity["starttime"]
        except FileNotFoundError:
            return False

    def close(self, timeout=7):
        if self.alive() and self.matches():
            self.process.terminate()
            try:
                self.process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                self.forced = True
                if self.matches():
                    self.process.kill()
                self.process.wait(timeout=3)
        else:
            self.process.wait(timeout=3)
        self.stop_result = self.process.returncode
        self.stdout.close()
        self.stderr.close()
        return {"pid": self.process.pid, "starttime": self.identity["starttime"], "returncode": self.stop_result, "forced": self.forced, "reaped": not self.alive()}


def log_bytes(root):
    return sum(p.stat().st_size for p in pathlib.Path(root).rglob("*") if p.is_file() and p.suffix in (".stdout", ".stderr"))


def log_guard(root, limit=LOG_LIMIT):
    size = log_bytes(root)
    demand(size <= limit, "log limit exceeded")
    return size


def ready(server, root, deadline):
    while time.monotonic() < deadline:
        demand(server.alive(), "server startup failed")
        log_guard(root)
        text = server.stdout_path.read_text(errors="replace")
        match = re.search(r"listening on port ([0-9]+)\.", text)
        if match:
            port = int(match.group(1))
            demand(server.matches() and listener_owned(server.process.pid, port), "readiness PID/port ownership mismatch")
            return port
        time.sleep(.02)
    raise Invalid("server readiness watchdog")


def resources_ok(output):
    memory = {line.split(":")[0]: int(line.split()[1]) for line in pathlib.Path("/proc/meminfo").read_text().splitlines()}
    free = shutil.disk_usage(output).free
    nofile = resource.getrlimit(resource.RLIMIT_NOFILE)[0]
    demand(memory["MemAvailable"] >= 1048576, "less than 1GiB MemAvailable")
    demand(free >= MIN_FREE_DISK, "less than 4GiB disk free")
    demand(nofile == resource.RLIM_INFINITY or nofile >= 256, "nofile below 256")
    return {"MemAvailable_kib": memory["MemAvailable"], "disk_free_bytes": free, "nofile": nofile}


def environment(output):
    cpu = pathlib.Path("/proc/cpuinfo").read_text()
    model = next(line.split(":", 1)[1].strip() for line in cpu.splitlines() if line.startswith("model name"))
    memory = next(line for line in pathlib.Path("/proc/meminfo").read_text().splitlines() if line.startswith("MemTotal:"))
    os_release = next(line.split("=", 1)[1].strip('"') for line in pathlib.Path("/etc/os-release").read_text().splitlines() if line.startswith("PRETTY_NAME="))
    return {"utc": utc(), "kernel": os.uname().release, "os": os_release, "WSL": "microsoft" in os.uname().release.lower(), "cpu_model": model, "online_cpus": os.cpu_count(), "affinity": sorted(os.sched_getaffinity(0)), "memory": memory, "preflight": resources_ok(output), "working_directory_category": "repository-local native Linux filesystem", "clock": "monotonic intervals / UTC wall timestamps", "limitations": "same-host loopback, WSL, closed-loop wrk, warmed page cache; timed responses not individually body-audited"}


def wrk_run(tool, server, port, payload, seconds, prefix, output, deadline):
    command = [str(tool), "-t2", "-c32", "--timeout", "2s", "--latency", "-d", f"{seconds}s", "-s", str(HERE / "summary.lua"), f"http://127.0.0.1:{port}/{payload['name']}"]
    initial = process_info(server.process.pid)
    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    initial_client_cpu = usage.ru_utime + usage.ru_stime
    began = time.monotonic()
    began_utc = utc()
    worker = OwnedProcess(command, prefix)
    samples = []
    next_sample = began
    try:
        while True:
            now = time.monotonic()
            demand(server.alive() and server.matches(), "server exited during wrk")
            demand(now <= min(deadline, began + seconds + 10), "wrk/global watchdog")
            log_guard(output)
            if now >= next_sample:
                samples.append({"elapsed": now - began, **process_info(server.process.pid)})
                next_sample += 1
            if not worker.alive():
                break
            time.sleep(.05)
        end = time.monotonic()
        final = process_info(server.process.pid)
        samples.append({"elapsed": end - began, **final})
        worker.process.wait()
        usage = resource.getrusage(resource.RUSAGE_CHILDREN)
        client_cpu = usage.ru_utime + usage.ru_stime - initial_client_cpu
        metrics = parse_summary(worker.stdout_path.read_text(), worker.process.returncode, seconds, payload["size"])
        interval = end - began
        demand(seconds * .8 <= interval <= seconds + 10 and abs(interval - metrics["elapsed_seconds"]) <= 3, "wrk wall duration mismatch")
        demand(all(item["rss_kib"] is not None and item["hwm_kib"] is not None for item in samples), "server resource sample missing")
        cpu = final["cpu_seconds"] - initial["cpu_seconds"]
        metrics["resources"] = {"envelope_start_utc": began_utc, "envelope_end_utc": utc(), "envelope_seconds": interval, "server_cpu_seconds": cpu, "server_cpu_percent_one_core": cpu / interval * 100, "wrk_cpu_seconds": client_cpu, "wrk_cpu_percent_one_core": client_cpu / interval * 100, "rss_sampled_max_kib": max(item["rss_kib"] for item in samples), "VmHWM_kib_including_warmup": final["hwm_kib"], "rss_samples": samples, "rss_sample_count": len(samples)}
        return metrics
    finally:
        save(str(prefix) + ".cleanup.json", worker.close())


def invariant(manifest, root, payload):
    demand(sha(manifest["binary"]) == manifest["binary_sha256"], "server binary changed during sample")
    demand(sha(pathlib.Path(root) / payload["name"]) == payload["sha256"], "fixture changed during sample")


def run_sample(manifest, tool, root, payload, directory, output, deadline, warmup=5, duration=20):
    directory.mkdir()
    row = {"status": "invalid", "label": manifest["label"], "commit": manifest["commit"], "payload": payload, "started_utc": utc(), "warmup_seconds": warmup, "measurement_seconds": duration}
    command = [manifest["binary"], *SERVER_ARGS, "--root", str(root)]
    row["server_command"] = command
    row["config_sha256"] = hashlib.sha256(json.dumps(command).encode()).hexdigest()
    server = None
    try:
        invariant(manifest, root, payload)
        server = OwnedProcess(command, directory / "server")
        row["server_identity"] = server.identity
        port = ready(server, output, min(deadline, time.monotonic() + 5))
        row["port"] = port
        row["pre_audit"] = audit(port, payload)
        row["warmup"] = wrk_run(tool, server, port, payload, warmup, directory / "warmup", output, deadline)
        row["measurement"] = wrk_run(tool, server, port, payload, duration, directory / "measurement", output, deadline)
        row["post_audit"] = audit(port, payload)
        invariant(manifest, root, payload)
        demand(server.alive(), "server exited after measurement")
        row["status"] = "valid"
    except BaseException as error:
        row["error"] = type(error).__name__ + ": " + str(error)
        raise
    finally:
        if server:
            cleanup = server.close()
            row["cleanup"] = cleanup
            if cleanup["forced"] or cleanup["returncode"] != 0:
                row["status"] = "invalid"
                row.setdefault("error", "server required force or exited abnormally")
        row["ended_utc"] = utc()
        save(directory / "sample.json", row)
    demand(row["status"] == "valid", row.get("error", "sample invalid"))
    return row


def schedule():
    return [(round_no, size, label) for round_no in (1, 2, 3) for size in ((1024, 1048576) if round_no % 2 else (1048576, 1024)) for label in (("A", "B") if round_no % 2 else ("B", "A"))]


def aggregate(rows):
    demand(len(rows) == 12 and all(row["status"] == "valid" for row in rows), "baseline incomplete or invalid")
    groups = {}
    for size in (1024, 1048576):
        for label in ("A", "B"):
            group = [row for row in rows if row["label"] == label and row["payload"]["size"] == size]
            demand(len(group) == 3, "group missing original three samples")
            values = [row["measurement"]["qps"] for row in group]
            median = statistics.median(values)
            spread = (max(values) - min(values)) / median
            groups[f"{label}-{size}"] = {"qps_samples": values, "qps_median": median, "qps_min": min(values), "qps_max": max(values), "qps_relative_span": spread, "noisy": spread > .2, "median_of_run_p99_ms": statistics.median(row["measurement"]["latency_ms"]["p99"] for row in group)}
    ratios = {str(size): groups[f"B-{size}"]["qps_median"] / groups[f"A-{size}"]["qps_median"] for size in (1024, 1048576)}
    return {"groups": groups, "B_over_A_qps_median": ratios}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-manifest", required=True)
    parser.add_argument("--candidate-manifest", required=True)
    parser.add_argument("--wrk", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--smoke", action="store_true", help="separate non-baseline experiment: one B/1KiB sample, 1s warmup + 1s measurement")
    args = parser.parse_args(argv)
    output = pathlib.Path(args.output).resolve()
    demand(output.is_relative_to(HERE.parent), "output must be inside repository task root")
    output.mkdir(parents=True, exist_ok=True)
    demand(not any(output.iterdir()), "output directory is not empty")
    result = {"status": "invalid", "experiment": "smoke" if args.smoke else "V0.5-S4-fixed", "samples": [],
              "log_limit_bytes": LOG_LIMIT, "min_free_disk_bytes": MIN_FREE_DISK}
    start = time.monotonic()
    try:
        result["environment"] = environment(output)
        manifests = {"A": validate_manifest(args.baseline_manifest, "A"), "B": validate_manifest(args.candidate_manifest, "B")}
        demand(manifests["A"]["compiler"] == manifests["B"]["compiler"] and manifests["A"]["cmake"] == manifests["B"]["cmake"], "toolchains differ")
        result["builds"] = manifests
        tool = validate_tool(args.wrk)
        result["tool"] = tool
        root = output / "root"
        root.mkdir()
        payloads = {size: fixture(root, size) for size in (1024, 1048576)}
        plan = [(1, 1024, "B")] if args.smoke else schedule()
        result["order"] = plan
        save(output / "run.json", result)
        for index, (round_no, size, label) in enumerate(plan, 1):
            demand(time.monotonic() - start <= 600, "global benchmark wall budget")
            row = run_sample(manifests[label], args.wrk, root, payloads[size], output / f"sample-{index:02d}-{label}-{size}", output, start + 600, 1 if args.smoke else 5, 1 if args.smoke else 20)
            row["round"] = round_no
            result["samples"].append(row)
            save(output / "run.json", result)
            print(f"sample {index}/{len(plan)} {label}/{size} valid qps={row['measurement']['qps']:.2f}", flush=True)
        validate_manifest(args.baseline_manifest, "A")
        validate_manifest(args.candidate_manifest, "B")
        final_tool = validate_tool(args.wrk)
        demand(final_tool["sha256"] == tool["sha256"] and final_tool["libraries"] == tool["libraries"], "wrk or dependencies changed")
        log_guard(output)
        if not args.smoke:
            result["summary"] = aggregate(result["samples"])
        result["status"] = "valid"
        return 0
    except (Exception, KeyboardInterrupt) as error:
        result["error"] = type(error).__name__ + ": " + str(error)
        print(result["error"], file=sys.stderr)
        return 1
    finally:
        result["wall_seconds"] = time.monotonic() - start
        result["ended_utc"] = utc()
        result["observed_log_bytes_at_stop"] = log_bytes(output)
        save(output / "run.json", result)
        # Only generated fixtures are removed; raw and existing user files remain.
        if (output / "root").is_dir():
            shutil.rmtree(output / "root")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Invalid as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
