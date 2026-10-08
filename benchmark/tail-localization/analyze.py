"""Offline request timelines. Input is normalized evidence, never wrk buckets."""
import json
from collections import defaultdict


class Invalid(ValueError):
    pass


def interval_union(intervals):
    merged = []
    for start, end in sorted(intervals):
        if end < start:
            raise Invalid("reversed local interval")
        if merged and start <= merged[-1][1]:
            merged[-1][1] = max(end, merged[-1][1])
        else:
            merged.append([start, end])
    return merged


def intersection_duration(intervals, start, end):
    return sum(max(0, min(right, end) - max(left, start))
               for left, right in interval_union(intervals))


def analyze(document):
    metadata = document["metadata"]
    if metadata.get("overflow") or not metadata.get("writers_stopped"):
        raise Invalid("buffer overflow or active writer")
    if metadata.get("clock") != "CLOCK_MONOTONIC" or metadata.get("unit") != "ns":
        raise Invalid("unsupported clock")
    endpoints = metadata["endpoints"]
    for field in ("boot_id", "time_namespace"):
        values = [endpoint.get(field) for endpoint in endpoints]
        if len(values) != 2 or not all(values) or len(set(values)) != 1:
            raise Invalid("endpoint clock identity differs: " + field)
    connections = {}
    for connection in document["connections"]:
        key = (connection["run_id"], tuple(connection["four_tuple"]), connection["lifetime"])
        if key in connections or not connection.get("frozen_before_warmup"):
            raise Invalid("duplicate or late connection mapping")
        if not connection.get("one_inflight") or connection.get("reconnect"):
            raise Invalid("unsupported client flow")
        for endpoint in ("server", "client"):
            for field in ("pid", "starttime", "tid", "worker"):
                value = connection[endpoint].get(field)
                if type(value) is not int or value < (0 if field == "worker" else 1):
                    raise Invalid("missing/invalid endpoint owner")
        connections[key] = connection
    if len(connections) > 16:
        raise Invalid("selection exceeds 16 connections")
    grouped = defaultdict(list)
    sequences = defaultdict(set)
    endpoint_kinds = {
        "client": {"write_begin", "write_complete", "first_byte", "client_complete", "activity"},
        "server": {"server_first_read", "parse_complete", "response_enqueued", "output_drained", "handler_return", "activity"},
    }
    for event in document["events"]:
        key = (event["run_id"], tuple(event["four_tuple"]), event["lifetime"])
        if key not in connections or not isinstance(event["time_ns"], int):
            raise Invalid("unmapped event or invalid timestamp")
        connection = connections[key]
        sequence = event["request_sequence"]
        if type(sequence) is not int or sequence < 1:
            raise Invalid("invalid request sequence")
        if event["kind"] not in endpoint_kinds.get(event["endpoint"], set()):
            raise Invalid("event endpoint/kind mismatch")
        sequences[key].add(sequence)
        identity = connection[event["endpoint"]]
        for field in ("pid", "starttime", "tid", "worker"):
            if event.get(field) != identity.get(field):
                raise Invalid("event owner identity mismatch: " + field)
        grouped[key + (event["request_sequence"],)].append(event)
    for values in sequences.values():
        ordered = sorted(values)
        if any(right != left + 1 for left, right in zip(ordered, ordered[1:])):
            raise Invalid("request sequence gap")
    result = []
    for key, events in grouped.items():
        point = {}
        activities = []
        for event in events:
            kind = event["kind"]
            if kind == "activity":
                activities.append((event["time_ns"], event["end_ns"]))
                continue
            if kind in point:
                raise Invalid("duplicate request event: " + kind)
            point[kind] = event
        required = ("write_begin", "write_complete", "first_byte", "client_complete",
                    "server_first_read", "parse_complete", "response_enqueued", "output_drained",
                    "handler_return")
        missing = [kind for kind in required if kind not in point]
        item = {"key": list(key), "complete": not missing, "missing": missing}
        if missing:
            expected_key = [key[0], list(key[1]), key[2], key[3]]
            boundaries = [entry for entry in document.get("request_boundaries", []) if entry.get("key") == expected_key]
            if len(boundaries) != 1:
                raise Invalid("missing unique full-key boundary evidence")
            boundary = boundaries[0]
            reason = boundary.get("reason")
            valid_start = reason == "active_at_recording_start" and boundary.get("active_start_ns", float("inf")) < metadata.get("recording_start_ns", -1)
            valid_end = reason == "active_at_recording_end" and boundary.get("active_end_ns", -1) > metadata.get("recording_end_ns", float("inf"))
            if not (valid_start or valid_end):
                raise Invalid("missing interior event without recording-boundary evidence")
            item["classification"] = "boundary_unknown"
            result.append(item)
            continue
        times = {kind: value["time_ns"] for kind, value in point.items()}
        for order in (("write_begin", "write_complete", "first_byte", "client_complete"),
                      ("server_first_read", "parse_complete", "response_enqueued", "output_drained", "handler_return")):
            if any(times[left] > times[right] for left, right in zip(order, order[1:])):
                raise Invalid("endpoint-local event order")
        complete = point["client_complete"]
        if not complete.get("content_length_verified") or not complete.get("body_verified"):
            raise Invalid("response body not verified")
        total = times["client_complete"] - times["write_begin"]
        submit = times["write_complete"] - times["write_begin"]
        # Signed cross-endpoint boundaries are evidence, not additive components.
        item.update(raw_latency_ns=total, slow=total >= 50_000_000,
                    client_submit_ns=submit,
                    write_complete_to_server_read_ns=times["server_first_read"] - times["write_complete"],
                    server_read_to_drained_ns=times["output_drained"] - times["server_first_read"],
                    drained_to_client_complete_ns=times["client_complete"] - times["output_drained"],
                    activity_union_in_request_ns=intersection_duration(activities, times["write_begin"], times["client_complete"]),
                    residual_unknown_ns=total - intersection_duration(activities, times["write_begin"], times["client_complete"]),
                    phase="measurement" if metadata["measurement_start_ns"] <= times["write_begin"] and
                    times["client_complete"] <= metadata["measurement_end_ns"] else "boundary")
        result.append(item)
    return {"schema": 1, "requests": result, "slow_requests": [item for item in result if item.get("slow")],
            "measurement_slow_requests": [item for item in result if item.get("slow") and item.get("phase") == "measurement"],
            "limits": ["cross-endpoint signed spans are not additive", "residual is unknown, not CPU or network", "normalized schema requires binary decoder validation"]}


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("input")
    parser.add_argument("output")
    args = parser.parse_args()
    with open(args.input, encoding="utf-8") as source:
        output = analyze(json.load(source))
    with open(args.output, "x", encoding="utf-8") as target:
        json.dump(output, target, indent=2)
        target.write("\n")
