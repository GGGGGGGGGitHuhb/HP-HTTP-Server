"""Scenario-aware process executor, based on frozen audited benchmark primitives."""
import hashlib
import json
import resource
import time
from identity import legacy
from model import server_args, wrk_args
from run import OwnedProcess, process_info, utc, demand, log_guard, parse_summary, save, HERE

def wrk_run(tool, server, port, payload, seconds, prefix, output, deadline, item):
    command = [str(tool), *wrk_args(item), "--timeout", "2s", "--latency", "-d", f"{seconds}s", "-s", str(HERE / "summary.lua"), f"http://127.0.0.1:{port}/{payload['name']}"]
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
        metrics["command"] = command
        return metrics
    finally:
        cleanup = worker.close()
        save(str(prefix) + ".cleanup.json", cleanup)
        demand(not cleanup["forced"] and cleanup["reaped"], "wrk forced/unreaped cleanup")


def run_sample(manifest, tool, root, payload, directory, output, deadline, item, warmup=5, duration=20):
    directory.mkdir()
    row = {"status": "invalid", "label": manifest["label"], "commit": manifest["commit"], "payload": payload, "started_utc": utc(), "warmup_seconds": warmup, "measurement_seconds": duration}
    command = [manifest["binary"], *server_args(item), "--root", str(root)]
    row["schedule"] = item
    row["server_command"] = command
    row["config_sha256"] = hashlib.sha256(json.dumps(command).encode()).hexdigest()
    server = None
    try:
        legacy.invariant(manifest, root, payload)
        server = OwnedProcess(command, directory / "server")
        row["server_identity"] = server.identity
        port = legacy.ready(server, output, min(deadline, time.monotonic() + 5))
        row["port"] = port
        row["pre_audit"] = legacy.audit(port, payload)
        row["warmup"] = wrk_run(tool, server, port, payload, warmup, directory / "warmup", output, deadline, item)
        row["measurement"] = wrk_run(tool, server, port, payload, duration, directory / "measurement", output, deadline, item)
        row["post_audit"] = legacy.audit(port, payload)
        legacy.invariant(manifest, root, payload)
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
