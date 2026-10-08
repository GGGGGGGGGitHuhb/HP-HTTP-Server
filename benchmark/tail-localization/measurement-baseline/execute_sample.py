"""R015 普通 HTTP 样本：相同128连接 warmup+测量，不创建观测namespace。"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import stat
import subprocess
import sys
import time

sys.dont_write_bytecode = True
PACKAGE = Path(__file__).absolute().parent

for source_path in (Path(__file__).absolute(), PACKAGE / "prepare_package.py", PACKAGE / "owned_process.py", PACKAGE / "inputs-lock.json"):
    for ancestor in reversed(source_path.parents):
        if stat.S_ISLNK(ancestor.lstat().st_mode):
            raise ValueError("package import parent symlink")
    if not stat.S_ISREG(source_path.lstat().st_mode):
        raise ValueError("package import source not regular")
lock_bytes = (PACKAGE / "inputs-lock.json").read_bytes()
if hashlib.sha256(lock_bytes).hexdigest() != os.environ.get("HP_BASELINE_PACKAGE_SHA256"):
    raise ValueError("package license SHA missing/drift before import")
declared_sources = {entry["path"]: entry for entry in json.loads(lock_bytes)["files"]}
for import_name in ("prepare_package.py", "owned_process.py", "execute_sample.py"):
    if hashlib.sha256((PACKAGE / import_name).read_bytes()).hexdigest() != declared_sources[import_name]["sha256"]:
        raise ValueError("package module drift before import")

def load_own(name):
    spec = importlib.util.spec_from_file_location("baseline_" + name, PACKAGE / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

prepare = load_own("prepare_package")
owned = load_own("owned_process")


def digest(path):
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        if not stat.S_ISREG(os.fstat(descriptor).st_mode):
            raise ValueError("catalog non-regular file")
        checksum = hashlib.sha256()
        while True:
            block = os.read(descriptor, 1024 * 1024)
            if not block:
                break
            checksum.update(block)
        return checksum.hexdigest()
    finally:
        os.close(descriptor)


def run_sample(args):
    prepare.verify_package(PACKAGE)
    output = prepare.validate_directory_path(args.output_root)
    build_output = prepare.validate_directory_path(args.build_output)
    if os.environ.get("HP_BASELINE_ROLE") != args.role or os.environ.get("HP_BASELINE_RUN") != args.run_id:
        raise ValueError("execution role/run license missing")
    if os.environ.get("HP_BASELINE_OUTPUT_ROOT") != str(output):
        raise ValueError("execution output license mismatch")
    if digest(PACKAGE / "inputs-lock.json") != os.environ.get("HP_BASELINE_PACKAGE_SHA256"):
        raise ValueError("execution package identity mismatch")
    parent = owned.process_identity(os.getppid())
    if not parent or parent["pid"] != int(os.environ["HP_BASELINE_GOVERNOR_PID"]) or parent["starttime"] != int(os.environ["HP_BASELINE_GOVERNOR_STARTTIME"]):
        raise ValueError("governor identity mismatch")
    work_deadline = float(os.environ["HP_BASELINE_WORK_DEADLINE"])
    outer_deadline = float(os.environ["HP_BASELINE_CLEANUP_DEADLINE"])
    if not time.monotonic() < work_deadline < outer_deadline:
        raise ValueError("execution absolute deadline invalid")
    receipt_path = build_output / "build-receipt.json"
    if digest(receipt_path) != os.environ["HP_BASELINE_BUILD_RECEIPT_SHA256"]:
        raise ValueError("build receipt binding mismatch")
    receipt = json.loads(receipt_path.read_text())
    build = receipt["builds"][1]
    if build["packageRoot"] != str(PACKAGE) or build["serverSourceCommit"] != "acda3f92d42a36d0b0554e185bc6f4155b4e5889":
        raise ValueError("relocation/source mismatch")
    binary_root = prepare.validate_directory_path(build["outputRoot"])
    server_binary = binary_root / "build-E/hp_http_server"
    client_binary = binary_root / "client/wrk-baseline"
    if digest(server_binary) != build["serverSha256"] or digest(client_binary) != build["clientSha256"]:
        raise ValueError("binary drift")
    dependency_identity = []
    for binary in (server_binary, client_binary):
        for dependency in build["actualLoadedLibraries"][str(binary)]["files"]:
            if digest(dependency["path"]) != dependency["sha256"]:
                raise ValueError("actual runtime DSO drift")
            if dependency not in dependency_identity:
                dependency_identity.append(dependency)
    with (output / "build-receipt.json").open("xb") as stream:
        stream.write(receipt_path.read_bytes())
    config = json.loads((PACKAGE / "config/workload.json").read_text())
    document_root = output / "document-root"
    document_root.mkdir(exist_ok=False)
    payload = (PACKAGE / "config/payload-1024.bin").read_bytes()
    if len(payload) != 1024 or hashlib.sha256(payload).hexdigest() != config["payload_sha256"]:
        raise ValueError("payload identity mismatch")
    (document_root / "payload-1024.bin").write_bytes(payload)
    result = {"schema": "baseline-v1", "role": args.role, "run_id": args.run_id, "status": "invalid",
              "uid": os.getuid(), "gid": os.getgid(), "runner_identity": owned.process_identity(os.getpid()),
              "package_sha256": digest(PACKAGE / "inputs-lock.json"), "first_error": None,
              "server_sha256": build["serverSha256"], "client_sha256": build["clientSha256"],
              "source_commit": build["serverSourceCommit"], "build_receipt_sha256": digest(receipt_path),
              "payload_sha256": config["payload_sha256"],
              "request_sha256": digest(PACKAGE / "config/request.bin"),
              "request_bytes": (PACKAGE / "config/request.bin").stat().st_size,
              "resources": [],
              "runtime_dependencies": dependency_identity,
              "cleanup_errors": [], "processes": [], "cleanup": [], "catalog": []}
    resources = []
    streams = []
    saved_handlers = {}
    environment = {name: value for name, value in os.environ.items()
                   if name.startswith("HP_BASELINE_") or name in ("TMPDIR", "TMP", "TEMP", "XDG_CACHE_HOME", "PYTHONDONTWRITEBYTECODE")}
    environment["PATH"] = "/usr/bin:/bin"
    environment.update(LD_LIBRARY_PATH=str(PACKAGE / "runtime"),
                       LUA_PATH=str(PACKAGE / "runtime/lua/?.lua") + ";" + str(PACKAGE / "runtime/lua/?/init.lua"), LUA_CPATH="",
                       HP_BASELINE_WARMUP_SECONDS="1" if args.run_id == "run-r015-smoke-001" else "5",
                       HP_BASELINE_OUTPUT_DIR=str(output))
    result["environment"] = {name: environment[name] for name in
                             ("PATH", "TMPDIR", "TMP", "TEMP", "XDG_CACHE_HOME", "PYTHONDONTWRITEBYTECODE", "LD_LIBRARY_PATH", "LUA_PATH", "LUA_CPATH")}

    def record_error(error, operation):
        result["status"] = "invalid"
        entry = {"operation": operation, "type": type(error).__name__, "errno": getattr(error, "errno", None), "message": str(error)}
        if result["first_error"] is None:
            result["first_error"] = entry
        else:
            result["cleanup_errors"].append(entry)

    def spawn(name, command):
        stdout = (output / (name + ".stdout")).open("xb")
        streams.append(stdout)
        stderr = (output / (name + ".stderr")).open("xb")
        streams.append(stderr)
        process = subprocess.Popen(command, env=environment, stdout=stdout, stderr=stderr,
                                   cwd=PACKAGE, start_new_session=True)
        owner = owned.OwnedProcess(process, None)
        resources.append(owner)
        identity = owned.process_identity(process.pid)
        owner.identity = identity
        if identity is None:
            raise RuntimeError("launched process identity unavailable")
        result["processes"].append({"name": name, "identity": identity, "argv": command})
        result["resources"].append({"name": name, "identity": identity, "samples": [],
                                    "scope": "sampled_process_envelope_not_measurement_only_or_final_CPU"})
        capture_resources()
        return process

    def capture_resources():
        for row in result["resources"]:
            sample = owned.sample_resources(row["identity"])
            if sample is None:
                row["exited_before_final_proc_sample"] = True
            else:
                row["samples"].append(sample)

    def interrupted(number, frame):
        raise TimeoutError("outer governor requested bounded stop")

    try:
        for number in (signal.SIGTERM, signal.SIGINT):
            saved_handlers[number] = signal.signal(number, interrupted)
        server = spawn("server", [str(server_binary), "--threads", "4", "--idle-timeout-ms", "30000",
                                 "--keep-alive-timeout-ms", "15000", "--shutdown-timeout-ms", "5000",
                                 "--port", "0", "--root", str(document_root)])
        startup_deadline = min(work_deadline, time.monotonic() + 3)
        port = None
        while time.monotonic() < startup_deadline:
            if server.poll() is not None:
                raise RuntimeError("server stopped during startup")
            with (output / "server.stdout").open("rb") as stream:
                text = stream.read(65537)
            if len(text) > 65536:
                raise ValueError("startup log capacity exceeded")
            matches = re.findall(rb"listening on port (\d+)\.", text)
            if len(matches) > 1:
                raise ValueError("ambiguous listen identity")
            if matches:
                port = int(matches[0])
                break
            time.sleep(0.005)
        if port is None or not 0 < port <= 65535:
            raise RuntimeError("server port not published")
        duration = "3s" if args.run_id == "run-r015-smoke-001" else "20s"
        client = spawn("client", [str(client_binary), "-t2", "-c128", "-d", duration, "--timeout", "2s",
                                 f"http://127.0.0.1:{port}/payload-1024.bin"])
        next_resource_sample = time.monotonic() + 1
        while client.poll() is None:
            if server.poll() is not None:
                raise RuntimeError("server exited during workload")
            if time.monotonic() >= work_deadline:
                raise TimeoutError("absolute work deadline")
            if time.monotonic() >= next_resource_sample:
                capture_resources()
                next_resource_sample += 1
            time.sleep(0.005)
        if client.returncode != 0:
            raise RuntimeError("baseline client invalid; original failure evidence retained")
        observed = json.loads((output / "client.json").read_text())
        expected_duration = 3000000000 if duration == "3s" else 20000000000
        if observed["status"] != "valid" or observed["duration_ns"] != expected_duration:
            raise ValueError("client window invalid")
        if (observed["pid"], observed["uid"], observed["gid"]) != (client.pid, os.getuid(), os.getgid()):
            raise ValueError("actual client owner mismatch")
        if observed["request_sha256"] != result["request_sha256"] or observed["request_bytes"] != result["request_bytes"]:
            raise ValueError("actual request bytes mismatch")
        if observed["actual_LD_LIBRARY_PATH"] != environment["LD_LIBRARY_PATH"] or observed["actual_LUA_PATH"] != environment["LUA_PATH"] or observed["actual_luajit_library"] != str(PACKAGE / "runtime/libluajit-5.1.so.2"):
            raise ValueError("actual client runtime search mismatch")
        prepare.verify_package(PACKAGE)
        for dependency in dependency_identity:
            if digest(dependency["path"]) != dependency["sha256"]:
                raise ValueError("runtime DSO drift after workload")
        result["client"] = observed
        result["status"] = "valid"
    except BaseException as error:
        record_error(error, "work")
    finally:
        cleanup_deadline = min(outer_deadline, time.monotonic() + 7)
        try:
            capture_resources()
        except BaseException as error:
            record_error(error, "final_resource_sample")
        for owner in reversed(resources):
            try:
                cleanup = owner.close(cleanup_deadline)
                result["cleanup"].append(cleanup)
                if cleanup["errors"] or cleanup["unknown"] or not cleanup["reaped"]:
                    record_error(RuntimeError("child cleanup incomplete"), "cleanup")
            except BaseException as error:
                record_error(error, "cleanup")
        for stream in streams:
            try:
                stream.close()
            except BaseException as error:
                record_error(error, "stream_close")
        for number, previous in saved_handlers.items():
            try:
                signal.signal(number, previous)
            except BaseException as error:
                record_error(error, "restore_handler")
        for row in result["resources"]:
            samples = row["samples"]
            row["sampled_rss_max_bytes"] = max((entry["rss_bytes"] for entry in samples), default=None)
            row["cpu_percent"] = None
            if len(samples) >= 2:
                first, last = samples[0], samples[-1]
                delta_ticks = (last["utime_ticks"] + last["stime_ticks"]) - (first["utime_ticks"] + first["stime_ticks"])
                elapsed_ns = last["observed_ns"] - first["observed_ns"]
                if delta_ticks < 0 or elapsed_ns <= 0:
                    record_error(ValueError("resource CPU envelope invalid"), "resource_summary")
                else:
                    row["cpu_percent"] = delta_ticks / first["clock_ticks_per_second"] * 1e9 / elapsed_ns * 100
                    row["cpu_observation_start_ns"] = first["observed_ns"]
                    row["cpu_observation_end_ns"] = last["observed_ns"]
        if result["first_error"] is not None:
            result["status"] = "invalid"
        try:
            measured_names = {"client.json", "client-failure.json", "server.stdout", "server.stderr", "client.stdout", "client.stderr", "build-receipt.json"}
            measured_names.update({"main-raw.bin", "measurement-start-raw.bin", "cross-warmup-raw.bin", "warmup-raw.bin", "after-window-raw.bin", "main-corrected.bin", "measurement-start-corrected.bin"})
            for path in [output / name for name in sorted(measured_names) if (output / name).exists()] + [document_root / "payload-1024.bin"]:
                try:
                    if path.is_symlink():
                        record_error(ValueError("output symlink"), "catalog")
                    elif path.is_file() and path.name != "sample.json":
                        result["catalog"].append({"path": path.relative_to(output).as_posix(), "bytes": path.stat().st_size, "sha256": digest(path)})
                except BaseException as error:
                    record_error(error, "catalog_entry")
        except BaseException as error:
            record_error(error, "catalog_listing")
        try:
            with (output / "sample.json").open("x") as stream:
                json.dump(result, stream, indent=2, allow_nan=False)
                stream.write("\n")
        except BaseException as error:
            record_error(error, "sample_evidence")
            try:
                sys.stderr.write(json.dumps({"status": "invalid", "first_error": result["first_error"], "cleanup_errors": result["cleanup_errors"]}) + "\n")
            except BaseException:
                pass
        return 0 if result["status"] == "valid" else 1

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--role", choices=("builder", "reviewer"), required=True)
    parser.add_argument("--run-id", choices=("run-r015-smoke-001", "run-r015-baseline-001"), required=True)
    parser.add_argument("--output-root", required=True)
    parser.add_argument("--build-output", required=True)
    raise SystemExit(run_sample(parser.parse_args()))
