"""GDB-only: capture joined wrk histogram before/after stats_correct."""
import gdb
import json
import os
import struct
import time
import zlib
from pathlib import Path

prefix = Path(os.environ['R006_CAPTURE_PREFIX'])

def capture(pointer, name):
    began = time.monotonic()
    inferior = gdb.selected_inferior()
    header = bytes(inferior.read_memory(pointer, 32))
    count, limit, minimum, maximum = struct.unpack('<4Q', header)
    if not (0 < limit * 8 <= 16 * 1024 * 1024 and minimum <= maximum < limit):
        raise RuntimeError('histogram bounds')
    data = bytes(inferior.read_memory(pointer + 32, limit * 8))
    bins = struct.unpack('<' + str(limit) + 'Q', data)
    if sum(bins) != count:
        raise RuntimeError('histogram count mismatch')
    Path(str(prefix) + '.' + name + '.zlib').write_bytes(zlib.compress(header + data, 1))
    return dict(count=count, limit=limit, minimum=minimum, maximum=maximum, capture_seconds=time.monotonic()-began)

class Returned(gdb.Breakpoint):
    def __init__(self, address, pointer, metadata):
        super().__init__('*' + hex(address), temporary=True, internal=True)
        self.pointer, self.metadata = pointer, metadata
    def stop(self):
        try:
            self.metadata['return_monotonic'] = time.monotonic()
            self.metadata['corrected'] = capture(self.pointer, 'corrected')
            self.metadata['capture_end_monotonic'] = time.monotonic()
            Path(str(prefix) + '.histogram.json').write_text(json.dumps(self.metadata, indent=2))
        except Exception as error:
            Path(str(prefix) + '.error.json').write_text(json.dumps({'error': str(error)}))
        return False

class Entry(gdb.Breakpoint):
    def stop(self):
        try:
            entry_time = time.monotonic()
            pointer = int(gdb.parse_and_eval('$rdi'))
            expected = int(gdb.parse_and_eval('$rsi'))
            if expected <= 0 or len(gdb.selected_inferior().threads()) != 1:
                raise RuntimeError('invalid interval or workers not joined')
            pid = gdb.selected_inferior().pid
            starttime = Path('/proc/' + str(pid) + '/stat').read_text().rsplit(')',1)[1].split()[19]
            metadata = dict(entry_monotonic=entry_time, starttime=starttime, expected=expected, pointer=pointer, pid=gdb.selected_inferior().pid,
                            threads=len(gdb.selected_inferior().threads()), raw=capture(pointer, 'raw'))
            stack = int(gdb.parse_and_eval('$rsp'))
            address = struct.unpack('<Q', bytes(gdb.selected_inferior().read_memory(stack, 8)))[0]
            Returned(address, pointer, metadata)
        except Exception as error:
            Path(str(prefix) + '.error.json').write_text(json.dumps({'error': str(error)}))
        return False

Entry('stats_correct', internal=True)
