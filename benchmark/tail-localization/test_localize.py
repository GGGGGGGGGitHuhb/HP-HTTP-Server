"""Synthetic counterexamples; run only through the approved S4 ledger."""
import copy
import unittest
from analyze import Invalid, analyze, interval_union


def evidence():
    server = dict(pid=10, starttime=100, tid=11, worker=0)
    client = dict(pid=20, starttime=200, tid=21, worker=0)
    connection = dict(run_id="fixture", four_tuple=["127.0.0.1", 7000, "127.0.0.1", 8000],
                      lifetime=1, frozen_before_warmup=True, one_inflight=True,
                      reconnect=False, server=server, client=client)
    events = []
    points = [("client", "write_begin", 0), ("client", "write_complete", 20),
              ("server", "server_first_read", 10), ("server", "parse_complete", 25),
              ("server", "response_enqueued", 30), ("client", "first_byte", 40),
              ("client", "client_complete", 60), ("server", "output_drained", 70),
              ("server", "handler_return", 80)]
    for endpoint, kind, time in points:
        events.append(dict(run_id=connection["run_id"], four_tuple=connection["four_tuple"],
                           lifetime=1, request_sequence=1, endpoint=endpoint, kind=kind,
                           time_ns=time * 1_000_000, **connection[endpoint]))
    events[6].update(content_length_verified=True, body_verified=True)
    return dict(metadata=dict(overflow=False, writers_stopped=True, clock="CLOCK_MONOTONIC", unit="ns",
                              endpoints=[dict(boot_id="boot", time_namespace="ns")] * 2,
                              recording_start_ns=0, recording_end_ns=100_000_000,
                              measurement_start_ns=0, measurement_end_ns=100_000_000),
                connections=[connection], events=events)


class TimelineTests(unittest.TestCase):
    def test_normal_cross_endpoint_overlap_is_signed(self):
        request = analyze(evidence())["requests"][0]
        self.assertEqual(request["write_complete_to_server_read_ns"], -10_000_000)
        self.assertEqual(request["drained_to_client_complete_ns"], -10_000_000)
        self.assertTrue(request["slow"])

    def test_nested_activity_is_not_double_counted(self):
        self.assertEqual(interval_union([(1, 8), (2, 4), (7, 10)]), [[1, 10]])

    def test_missing_tail_is_explicit_unknown(self):
        document = evidence()
        document["events"].pop()
        connection = document["connections"][0]
        document["request_boundaries"] = [dict(key=[connection["run_id"], connection["four_tuple"], 1, 1], reason="active_at_recording_end", active_end_ns=101_000_000)]
        self.assertEqual(analyze(document)["requests"][0]["classification"], "boundary_unknown")

    def test_missing_interior_is_invalid(self):
        document = evidence()
        document["events"].pop(3)
        with self.assertRaises(Invalid):
            analyze(document)

    def test_duplicate_event_fails(self):
        document = evidence()
        document["events"].append(copy.deepcopy(document["events"][0]))
        with self.assertRaises(Invalid):
            analyze(document)

    def test_same_fd_cannot_replace_lifetime(self):
        document = evidence()
        document["events"][0]["fd"] = 8
        document["events"][0]["lifetime"] = 2
        with self.assertRaises(Invalid):
            analyze(document)

    def test_wrong_thread_cannot_match(self):
        document = evidence()
        document["events"][0]["tid"] += 1
        with self.assertRaises(Invalid):
            analyze(document)

    def test_overflow_clock_and_body_fail_closed(self):
        for mutation in ("overflow", "clock", "body"):
            document = evidence()
            if mutation == "overflow":
                document["metadata"]["overflow"] = True
            elif mutation == "clock":
                document["metadata"]["endpoints"] = [dict(boot_id="a", time_namespace="ns"), dict(boot_id="b", time_namespace="ns")]
            else:
                document["events"][6]["body_verified"] = False
            with self.assertRaises(Invalid):
                analyze(document)


if __name__ == "__main__":
    unittest.main()
