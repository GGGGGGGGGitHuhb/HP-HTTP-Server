#!/usr/bin/env python3
"""S2 current-server black-box byte and lifecycle regression."""
import hashlib
import json
import os
import pathlib
import shutil
import socket
import struct
import sys
import tempfile
import time

REPO = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'benchmark'))
import run as bench


def connect(port):
    peer = socket.create_connection(('127.0.0.1', port), timeout=5)
    peer.settimeout(5)
    return peer


def request(peer, payload, close=False, half_close=False):
    policy = 'close' if close else 'keep-alive'
    peer.sendall(f'GET /{payload["name"]} HTTP/1.1\r\nHost: localhost\r\nConnection: {policy}\r\n\r\n'.encode())
    if half_close:
        peer.shutdown(socket.SHUT_WR)
    received = b''
    while b'\r\n\r\n' not in received:
        part = peer.recv(65536)
        assert part, 'premature header EOF'
        received += part
    header, body = received.split(b'\r\n\r\n', 1)
    assert header.startswith(b'HTTP/1.1 200 '), header
    fields = dict(line.split(b':', 1) for line in header.split(b'\r\n')[1:])
    assert int(fields[b'Content-Length']) == payload['size']
    content = bytearray(body)
    while len(content) < payload['size']:
        part = peer.recv(65536)
        assert part, 'premature body EOF'
        content.extend(part)
    assert len(content) == payload['size'], 'unexpected trailing bytes'
    assert hashlib.sha256(content).hexdigest() == payload['sha256']
    if close or half_close:
        assert peer.recv(1) == b'', 'expected drained close'


def main():
    temporary = pathlib.Path(os.environ['HP_S3_TEST_TMP_ROOT'])
    temporary.mkdir(parents=True, exist_ok=True)
    output = pathlib.Path(tempfile.mkdtemp(prefix='s2-http-', dir=temporary))
    root = output / 'root'
    root.mkdir()
    payloads = {size: bench.fixture(root, size) for size in (0, 1024, 1048576, 8 * 1048576)}
    results = []
    try:
        for workers in (0, 2):
            command = [str(pathlib.Path(sys.argv[1]).resolve()), *bench.SERVER_ARGS, '--root', str(root)]
            command[command.index('--threads') + 1] = str(workers)
            server = bench.OwnedProcess(command, output / f'server-{workers}')
            try:
                port = bench.ready(server, output, time.monotonic() + 5)
                for size in (1024, 1048576):
                    with connect(port) as peer:
                        for _ in range(10):
                            request(peer, payloads[size])
                        request(peer, payloads[size], close=True)
                    with connect(port) as peer:
                        request(peer, payloads[size], half_close=True)
                for size in (0, 8 * 1048576):
                    with connect(port) as peer:
                        request(peer, payloads[size], close=True)
                with connect(port) as reset:
                    reset.sendall(b'GET /payload-1048576.bin HTTP/1.1\r\nHost: localhost\r\n\r\n')
                    reset.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
                with connect(port) as peer:
                    request(peer, payloads[1024], close=True)
                assert server.alive(), 'RST killed server'
            finally:
                cleanup = server.close()
                results.append({'workers': workers, 'cleanup': cleanup})
            assert cleanup['returncode'] == 0 and not cleanup['forced'] and cleanup['reaped']
        bench.save(output / 'result.json', {'status': 'valid', 'cases': results})
        print('threads=0/2: 10x keepalive, 0/1KiB/1MiB/8MiB exact bytes, FIN, RST, close, SIGTERM passed')
        return 0
    except BaseException as error:
        bench.save(output / 'result.json', {'status': 'invalid', 'error': repr(error), 'cases': results})
        raise
    finally:
        shutil.rmtree(root)


if __name__ == '__main__':
    raise SystemExit(main())
