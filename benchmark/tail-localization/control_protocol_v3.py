"""Bounded schema-3 SOCK_SEQPACKET startup messages; no live sockets on import."""
import json
import socket
import time

MAX_MESSAGE = 65536


def encode_message(run_id, phase, kind, **fields):
    if phase not in ('frozen', 'running', 'aborted') or kind not in ('ready', 'go', 'abort'):
        raise ValueError('illegal phase/kind')
    if (phase, kind) not in (('frozen', 'ready'), ('running', 'go'), ('aborted', 'abort')):
        raise ValueError('phase/kind disagreement')
    payload = dict(fields, schema=3, runId=run_id, phase=phase, kind=kind)
    encoded = json.dumps(payload, allow_nan=False).encode()
    if len(encoded) > MAX_MESSAGE:
        raise ValueError('message too large')
    return encoded


def receive_message(channel, run_id, expected_kind, deadline, received_kinds):
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError('startup absolute deadline')
    channel.settimeout(remaining)
    raw, ancillary, flags, address = channel.recvmsg(MAX_MESSAGE)
    if flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC):
        raise ValueError('startup message truncated')
    if not raw:
        raise EOFError('peerClosedBeforeReady')
    message = json.loads(raw)
    if not isinstance(message, dict) or message.get('schema') != 3 or message.get('runId') != run_id:
        raise ValueError('startup schema/runId mismatch')
    kind = message.get('kind')
    phase = message.get('phase')
    if (phase, kind) not in (('frozen', 'ready'), ('running', 'go'), ('aborted', 'abort')):
        raise ValueError('startup illegal phase/kind')
    if kind in received_kinds or 'abort' in received_kinds:
        raise ValueError('startup duplicate or message after abort')
    received_kinds.add(kind)
    if kind == 'abort':
        raise RuntimeError('endpointAbort: ' + repr(message.get('first_failure', 'unknown')))
    if kind != expected_kind:
        raise ValueError('unexpected startup message')
    return message


def read_published_failure(path, deadline):
    """Read only a fully published slot; a dead partial writer remains unknown."""
    import ctypes
    import mmap
    import os
    from wire_types_v3 import Atomics, Control, plain
    descriptor = None
    mapping = None
    control = None
    try:
        descriptor = os.open(path, os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC)
        if os.fstat(descriptor).st_size != ctypes.sizeof(Control):
            return 'unknown'
        mapping = mmap.mmap(descriptor, ctypes.sizeof(Control))
        control = Control.from_buffer(mapping)
        if bytes(control.magic) != b'S4CTRL03' or control.version != 3:
            return 'unknown'
        atomics = Atomics()
        until = min(deadline, time.monotonic() + .02)
        while atomics.load(control.firstFailure, 'published') == 1 and time.monotonic() < until:
            time.sleep(.001)
        if atomics.load(control.firstFailure, 'published') != 2:
            return 'unknown'
        return dict(plain(control.firstFailure), connection=plain(control.firstFailure.connection))
    except (OSError, ValueError):
        return 'unknown'
    finally:
        if control is not None: del control
        if mapping is not None: mapping.close()
        if descriptor is not None: os.close(descriptor)
