"""独立重建 baseline-v1 raw/corrected 全桶；无 ledger 或旧平台依赖。"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import stat
import struct
import sys
sys.dont_write_bytecode = True

MAX_US = 2000000
MAX_BINS = 262144
UINT64_MAX = (1 << 64) - 1
NAMES = ("main-raw.bin", "measurement-start-raw.bin", "cross-warmup-raw.bin", "warmup-raw.bin",
         "after-window-raw.bin", "main-corrected.bin", "measurement-start-corrected.bin")


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def read_regular(path, maximum):
    path = Path(path).absolute()
    for parent in path.parents:
        require(not stat.S_ISLNK(parent.lstat().st_mode), "symlink parent")
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        metadata = os.fstat(descriptor)
        require(stat.S_ISREG(metadata.st_mode) and metadata.st_size <= maximum, "file capacity/type")
        with os.fdopen(descriptor, "rb", closefd=False) as stream:
            value = stream.read(maximum + 1)
        require(len(value) == metadata.st_size, "file changed or oversized")
        after = os.fstat(descriptor)
        require((after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns) ==
                (metadata.st_dev, metadata.st_ino, metadata.st_size, metadata.st_mtime_ns), "file changed")
        return value
    finally:
        os.close(descriptor)


def decode_bins(value):
    require(len(value) % 16 == 0 and len(value) <= MAX_BINS * 16, "histogram record capacity")
    bins = {}
    previous = -1
    total = 0
    for latency, count in struct.iter_unpack("<QQ", value):
        require(previous < latency <= MAX_US and count > 0, "histogram bin order/count/range")
        total += count
        require(total <= UINT64_MAX, "histogram total overflow")
        bins[latency] = count
        previous = latency
    return bins


def reconstruct_correction(raw, interval):
    require(isinstance(interval, int) and 0 < interval <= ((1 << 63) - 1) // 2, "correction interval")
    corrected = dict(raw)
    maximum = max(raw, default=0)
    for latency in sorted(raw):
        if latency < 2 * interval:
            continue
        count = raw[latency]
        synthetic = latency - interval
        while synthetic > interval:
            corrected[synthetic] = corrected.get(synthetic, 0) + count
            require(corrected[synthetic] <= UINT64_MAX, "corrected bucket overflow")
            synthetic -= interval
    require(sum(corrected.values()) <= UINT64_MAX and max(corrected, default=0) == maximum,
            "corrected total/maximum")
    return corrected


def original_percentile(bins, scan_min, percentage):
    total = sum(bins.values())
    rank = math.floor((percentage / 100.0) * total + 1.0)
    accumulated = 0
    for latency, count in sorted(bins.items()):
        if latency < scan_min:
            continue
        accumulated += count
        if accumulated >= rank:
            return latency
    return 0


def verify_sample(directory):
    directory = Path(directory).absolute()
    sample_bytes = read_regular(directory / "sample.json", 16 * 1024 * 1024)
    sample = json.loads(sample_bytes)
    require(sample["schema"] == "baseline-v1-map-sample" and sample["status"] == "valid" and sample["first_error"] is None,
            "sample invalid")
    require(sample["run_id"] in ("run-r018-smoke-001", "run-r018-boundary-01", "run-r018-boundary-02", "run-r018-boundary-03"), "sample run")
    require(sample["role"] in ("builder", "reviewer") and
            (sample["role"] == "reviewer" or sample["run_id"] == "run-r018-smoke-001"), "role/sample scope")
    require(sample["source_commit"] == "acda3f92d42a36d0b0554e185bc6f4155b4e5889", "server source")
    package_root = Path(__file__).absolute().parent
    lock_bytes = read_regular(package_root / "inputs-lock.json", 4 * 1024 * 1024)
    require(sample["package_sha256"] == hashlib.sha256(lock_bytes).hexdigest(), "declared package identity drift")
    require(not sample["cleanup_errors"] and len(sample["cleanup"]) == 2 and
            all(row["reaped"] and not row["unknown"] and not row["errors"] for row in sample["cleanup"]),
            "cleanup incomplete")
    catalog = {}
    for entry in sample["catalog"]:
        relative = Path(entry["path"])
        require(not relative.is_absolute() and ".." not in relative.parts and entry["path"] not in catalog,
                "catalog path/duplicate")
        value = read_regular(directory / relative, 128 * 1024 * 1024)
        require(len(value) == entry["bytes"] and hashlib.sha256(value).hexdigest() == entry["sha256"], "raw catalog drift")
        catalog[entry["path"]] = value
    require(set(NAMES) <= catalog.keys() and {"client.json", "build-receipt.json", "document-root/payload-1024.bin"} <= catalog.keys(), "missing complete histogram/input receipt")
    require(hashlib.sha256(catalog["build-receipt.json"]).hexdigest() == sample["build_receipt_sha256"], "build receipt SHA")
    receipt = json.loads(catalog["build-receipt.json"])
    require(receipt["schema"] == "request-boundaries-build-v1" and receipt["inputs_lock_sha256"] == sample["package_sha256"], "R018 build receipt")
    mode = sample["mode"]
    require(mode in ("O", "B") and receipt["source_commit"] == sample["source_commit"] and
            receipt["server_" + mode]["sha256"] == sample["server_sha256"] and
            receipt["client_map"]["sha256"] == sample["client_sha256"], "actual binary receipt identity")
    require(hashlib.sha256(catalog["document-root/payload-1024.bin"]).hexdigest() == sample["payload_sha256"] ==
            "785b0751fc2c53dc14a4ce3d800e69ef9ce1009eb327ccf458afe09c242c26c9", "actual served fixture bytes")
    client = json.loads(catalog["client.json"])
    require(client["schema"] == "baseline-v1-client" and client["status"] == "valid", "client invalid")
    require(client["request_sha256"] == sample["request_sha256"] == hashlib.sha256(read_regular(package_root / "config/request.bin", 4096)).hexdigest()
            and client["request_bytes"] == sample["request_bytes"], "actual request identity")
    processes = sample["processes"]
    require(len(processes) == 2 and [row["name"] for row in processes] == ["server", "client"] and
            client["pid"] == processes[1]["identity"]["pid"] and client["uid"] == sample["uid"] and
            client["gid"] == sample["gid"], "actual process ownership")
    require(client["actual_LD_LIBRARY_PATH"] == sample["environment"]["LD_LIBRARY_PATH"] == receipt["relocated_package"] + "/runtime"
            and client["actual_LUA_PATH"] == sample["environment"]["LUA_PATH"] and ";;" not in client["actual_LUA_PATH"]
            and client["actual_luajit_library"] == receipt["relocated_package"] + "/runtime/libluajit-5.1.so.2", "actual runtime relocation")
    expected_dependencies = []
    for binary in (receipt["server_" + mode]["path"], receipt["client_map"]["path"]):
        for dependency in receipt["runtime_dependencies"][binary]["files"]:
            if dependency not in expected_dependencies:
                expected_dependencies.append(dependency)
    require(sample["runtime_dependencies"] == expected_dependencies, "actual DSO identity receipt")
    require(len(sample["resources"]) == 2, "CPU/RSS evidence missing")
    for resource, process in zip(sample["resources"], processes):
        require(resource["identity"] == process["identity"] and resource["name"] == process["name"] and
                resource["scope"] == "sampled_process_envelope_not_measurement_only_or_final_CPU", "CPU/RSS scope identity")
        samples = resource["samples"]
        require(len(samples) >= 2 and all(row["pid"] == process["identity"]["pid"] and row["starttime"] == process["identity"]["starttime"]
                and row["clock_ticks_per_second"] > 0 and min(row["utime_ticks"], row["stime_ticks"], row["rss_bytes"]) >= 0 for row in samples), "CPU/RSS sample bounds")
        first, last = samples[0], samples[-1]
        require(last["observed_ns"] > first["observed_ns"], "CPU envelope timestamps")
        percent = ((last["utime_ticks"] + last["stime_ticks"]) - (first["utime_ticks"] + first["stime_ticks"])) / first["clock_ticks_per_second"] * 1e9 / (last["observed_ns"] - first["observed_ns"]) * 100
        require(math.isfinite(resource["cpu_percent"]) and percent >= 0 and math.isclose(resource["cpu_percent"], percent, rel_tol=1e-12)
                and resource["sampled_rss_max_bytes"] == max(row["rss_bytes"] for row in samples), "CPU/RSS reduction")
    expected_duration = 3000000000 if sample["run_id"] == "run-r018-smoke-001" else 20000000000
    expected_warm = 1000000000 if sample["run_id"] == "run-r018-smoke-001" else 5000000000
    start, end = client["T0_ns"], client["T1_ns"]
    require(client["clock"] == "CLOCK_MONOTONIC" and end - start == client["duration_ns"] == expected_duration
            and start - client["warm_start_ns"] == expected_warm, "window mismatch")
    histograms = [decode_bins(catalog[name]) for name in NAMES]
    main, measured, cross = histograms[:3]
    require(sum(main.values()) == client["N"] > 0 and
            math.isfinite(client["qps"]) and math.isclose(client["qps"], client["N"] * 1e9 / expected_duration,
                                                         rel_tol=1e-12), "QPS/count denominator")
    require(main == {latency: measured.get(latency, 0) + cross.get(latency, 0)
                     for latency in measured.keys() | cross.keys()}, "cross/main partition")
    require(len(client["histograms"]) == 7 and len(client["interval_us"]) == 2, "histogram metadata size")
    for index in range(2):
        count = sum(histograms[index].values())
        require(count // 128 > 0, "zero correction denominator")
        interval = (expected_duration // 1000) // (count // 128)
        require(interval == client["interval_us"][index] > 0, "integer correction interval")
        require(reconstruct_correction(histograms[index], interval) == histograms[index + 5], "all corrected bins mismatch")
    for index, bins in enumerate(histograms):
        row = client["histograms"][index]
        raw = histograms[index - 5] if index >= 5 else bins
        scan_min = min(raw, default=UINT64_MAX)
        require(row["file"] == NAMES[index] and row["count"] == sum(bins.values()) and
                row["max_us"] == max(bins, default=0) and row["scan_min_us"] == scan_min, "histogram metadata")
        require(row["p50_us"] == original_percentile(bins, scan_min, 50) and
                row["p99_us"] == original_percentile(bins, scan_min, 99), "original percentile mismatch")
    owners = client["threads"]
    require(len(owners) == 2 and [row["owner"] for row in owners] == [0, 1], "thread ownership")
    completed = [0, 0, 0, 0]
    slow_count = 0
    slow_keys = set()
    slow_bins = {}
    for owner in owners:
        require(not owner["overflow"] and owner["stopped_ns"] >= end and len(owner["connections"]) == 64,
                "thread stopped/overflow/count")
        require(len(owner["slow_records"]) <= owner["slow_capacity"] == 16384, "slow capacity")
        require(all(value == 0 for value in owner["errors"].values()), "measurement error omitted")
        pending = 0
        pending_warm = 0
        censored = [[0, 0, 0], [0, 0, 0]]
        for position, connection in enumerate(owner["connections"]):
            require(connection["life"] == position + 1 and connection["ready_ns"] < client["warm_start_ns"]
                    and connection["closed"] and connection["end_reason"] in (1, 2, 3, 4), "connection final state")
            if connection["pending_at_T1"]:
                pending += 1
                pending_warm += connection["start_ns"] < start
                require(connection["start_ns"] < end and connection["wait_lower_bound_ns"] == end - connection["start_ns"],
                        "pending lower bound")
            reason = connection["end_reason"]
            if connection["sequence"] == 0:
                require(reason == 2 and connection["start_ns"] == 0 and connection["sent_bytes"] == 0,
                        "unused ready connection state")
            else:
                require(connection["sequence"] > 0 and client["warm_start_ns"] <= connection["start_ns"] < end,
                        "last actual request identity")
            if reason == 2:
                require(not connection["active"] and not connection["pending_at_T1"], "idle boundary state")
            else:
                require(connection["pending_at_T1"], "end reason missing pending snapshot")
                if reason == 3:
                    require(not connection["active"] and connection["full_sent"], "after-complete boundary state")
                elif reason == 1:
                    require(connection["active"] and not connection["full_sent"] and
                            connection["sent_bytes"] < client["request_bytes"], "incomplete-send censor")
                    censored[int(connection["start_ns"] >= start)][int(connection["sent_bytes"] > 0)] += 1
                else:
                    require(connection["active"] and connection["full_sent"] and connection["sent_bytes"] == client["request_bytes"] and
                            connection["start_ns"] + 2000000000 >= end, "deadline-censor boundary state")
                    censored[int(connection["start_ns"] >= start)][2] += 1
        require(pending == owner["pending_at_T1"] and pending_warm == owner["pending_warm"], "pending snapshot count")
        require(censored == owner["censored_by_start_phase"] and
                sum(row[0] for row in censored) == owner["censored_unsent"] and
                sum(row[1] for row in censored) == owner["censored_partial"] and
                sum(row[2] for row in censored) == owner["censored_sent_deadline_after_T1"], "censored state/phase counts")
        for index in range(4):
            completed[index] += owner["completed"][index]
        for record in owner["slow_records"]:
            key = (owner["owner"], record["life"], record["sequence"])
            require(key not in slow_keys and 1 <= record["life"] <= 64 and record["sequence"] > 0,
                    "slow identity duplicate")
            slow_keys.add(key)
            complete = record["complete_ns"]
            begin = record["start_ns"]
            require(start <= complete < end and client["warm_start_ns"] <= begin < end and complete - begin >= 50000000 and
                    complete - begin < 2000000000 and record["status"] == 200 and record["body_bytes"] == 1024,
                    "slow completion semantics")
            require(record["class"] == (1 if begin < start else 2), "slow phase")
            slow_count += 1
            bin_us = (complete - begin) // 1000
            slow_bins[bin_us] = slow_bins.get(bin_us, 0) + 1
    require(completed[1] + completed[2] == client["N"] and completed[0] == sum(histograms[3].values()) and
            completed[3] == sum(histograms[4].values()), "completed partition")
    require(completed[1] == sum(cross.values()) and completed[2] == sum(measured.values()), "cross/measured separate count")
    require(slow_count == sum(count for latency, count in main.items() if latency >= 50000), "complete slow records")
    require(slow_bins == {latency: count for latency, count in main.items() if latency >= 50000}, "all slow record bins match raw")
    return {"schema": "baseline-v1-association", "status": "valid", "role": sample["role"], "run_id": sample["run_id"],
            "sample_sha256": hashlib.sha256(sample_bytes).hexdigest(), "package_sha256": sample["package_sha256"],
            "N": client["N"], "duration_ns": expected_duration, "qps": client["qps"], "slow_count": slow_count,
            "main_raw_p99_us": client["histograms"][0]["p99_us"], "corrected_p99_us": client["histograms"][5]["p99_us"],
            "interval_us": client["interval_us"], "main_raw_max_us": client["histograms"][0]["max_us"],
            "cross_warmup_count": completed[1], "after_window_count": completed[3], "all_corrected_bins_verified": True}

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--sample-dir", action="append", required=True)
    parser.add_argument("--output-root", required=True)
    args = parser.parse_args()
    results = [verify_sample(path) for path in args.sample_dir]
    output = Path(args.output_root).absolute()
    with (output / "association.json").open("x") as stream:
        json.dump({"schema": "baseline-v1-offline", "status": "valid", "samples": results}, stream, indent=2, allow_nan=False)
        stream.write("\n")
