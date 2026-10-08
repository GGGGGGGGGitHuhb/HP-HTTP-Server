"""Strict, streaming validation of schema-1 thread files (no text hot path)."""
import hashlib
from pathlib import Path
import struct

HEADER = struct.Struct("<8sHHIQQIIQQQ")
RECORD = struct.Struct("<QQIIHHi")
MAGIC = b"S4TAIL01"
KINDS = {1: "client_write_begin", 2: "client_write_complete", 3: "client_first_byte", 4: "client_complete",
         5: "server_callback_begin", 6: "server_receive_begin", 7: "server_receive_return", 8: "server_parse_complete",
         9: "server_response_prepared", 10: "server_response_enqueued", 11: "server_send_begin", 12: "server_send_return",
         13: "server_sendfile_begin", 14: "server_sendfile_return", 15: "server_output_drained", 16: "server_handler_return"}


class DecodeError(ValueError):
    pass


def records(path, metadata):
    path = Path(path)
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        header_bytes = stream.read(HEADER.size)
        if len(header_bytes) != HEADER.size:
            raise DecodeError("short header")
        digest.update(header_bytes)
        magic, header_size, record_size, flags, count, capacity, pid, tid, starttime, first, last = HEADER.unpack(header_bytes)
        if magic != MAGIC or header_size != HEADER.size or record_size != RECORD.size or flags != 1:
            raise DecodeError("header layout/overflow/writer state")
        if count > capacity or path.stat().st_size != HEADER.size + count * RECORD.size:
            raise DecodeError("record length/count mismatch")
        if (pid, tid, starttime) != (metadata["pid"], metadata["tid"], metadata["starttime"]):
            raise DecodeError("thread identity mismatch")
        if metadata["endpoint"] not in ("server", "client"):
            raise DecodeError("unknown endpoint")
        previous = -1
        actual_first = actual_last = 0
        sequences = {}
        for index in range(count):
            raw = stream.read(RECORD.size)
            if len(raw) != RECORD.size:
                raise DecodeError("truncated record")
            digest.update(raw)
            timestamp, value, connection, sequence, kind, event_flags, result = RECORD.unpack(raw)
            if timestamp < previous or kind not in KINDS or sequence < 1:
                raise DecodeError("timestamp/kind/sequence invalid")
            if not metadata["recording_start_ns"] <= timestamp <= metadata["recording_end_ns"]:
                raise DecodeError("record outside recording clock window")
            if connection not in metadata["connection_ids"]:
                raise DecodeError("connection not frozen/mapped")
            if (kind <= 4) != (metadata["endpoint"] == "client"):
                raise DecodeError("kind endpoint mismatch")
            old_sequence = sequences.get(connection, sequence)
            if sequence < old_sequence or sequence > old_sequence + 1:
                raise DecodeError("request sequence regression/gap")
            sequences[connection] = sequence
            actual_first = timestamp if index == 0 else actual_first
            actual_last = timestamp
            previous = timestamp
            yield dict(time_ns=timestamp, value=value, connection_id=connection,
                       request_sequence=sequence, kind=KINDS[kind], flags=event_flags, result=result)
        if (first, last) != (actual_first, actual_last):
            raise DecodeError("header time range mismatch")
        if digest.hexdigest() != metadata["sha256"]:
            raise DecodeError("file SHA mismatch")
    # Caller must exhaust the iterator before accepting any partial result.
