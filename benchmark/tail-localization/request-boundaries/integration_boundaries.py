"""check槽内真实E接缝：分片、连续keepalive、header/file排空、部分EOF。"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import time
sys.dont_write_bytecode = True
ROOT = Path(__file__).absolute().parent
spec = importlib.util.spec_from_file_location('owned_boundary_integration', ROOT/'owned_process.py')
owned = importlib.util.module_from_spec(spec)
spec.loader.exec_module(owned)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--package-root', required=True)
    parser.add_argument('--build-output', required=True)
    parser.add_argument('--output-root', required=True)
    args = parser.parse_args()
    output = Path(args.output_root)
    expected = Path(os.environ['HP_BASELINE_OUTPUT_ROOT'])/'integration'
    if output != expected or Path(args.package_root) != ROOT:
        raise ValueError('integration exact licensed route')
    output.mkdir()
    document = output/'document-root'; document.mkdir()
    (document/'payload-1024.bin').write_bytes((ROOT/'config/payload-1024.bin').read_bytes())
    large = bytes(range(256))*32768
    (document/'large.bin').write_bytes(large)
    receipt = json.loads((Path(args.build_output)/'build-receipt.json').read_text())
    deadline = float(os.environ['HP_BASELINE_WORK_DEADLINE'])
    environment = {key:value for key,value in os.environ.items() if key.startswith('HP_BASELINE_') or key in ('TMPDIR','TMP','TEMP','XDG_CACHE_HOME','PYTHONDONTWRITEBYTECODE')}
    environment.update(PATH='/usr/bin:/bin', HP_BOUNDARY_OUTPUT_DIR=str(output), LD_LIBRARY_PATH=str(ROOT/'runtime'))
    error = None
    cleanup_errors = []
    cleanup = None
    channel = None
    process = None
    owner = None
    streams = []
    try:
        stdout = (output/'server.stdout').open('xb'); streams.append(stdout)
        stderr = (output/'server.stderr').open('xb'); streams.append(stderr)
        process = subprocess.Popen([receipt['server_B']['path'], '--threads','4','--port','0','--root',str(document),
            '--idle-timeout-ms','30000','--keep-alive-timeout-ms','15000','--shutdown-timeout-ms','5000'],
            env=environment, stdout=stdout, stderr=stderr, start_new_session=True)
        owner = owned.OwnedProcess(process, None)
        owner.identity = owned.process_identity(process.pid)
        if owner.identity is None: raise ValueError('integration child identity unavailable')
        port = None
        startup_deadline = min(deadline, time.monotonic()+3)
        while time.monotonic() < startup_deadline:
            if process.poll() is not None: raise RuntimeError('B startup exit')
            matches = re.findall(rb'listening on port (\d+)\.', (output/'server.stdout').read_bytes())
            if matches:
                if len(matches) != 1: raise ValueError('ambiguous listen')
                port = int(matches[0]); break
            time.sleep(.005)
        if port is None: raise TimeoutError('no B port')
        channel = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        channel.settimeout(min(10, max(.001, deadline-time.monotonic())))
        channel.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        channel.connect(('127.0.0.1',port))
        for resource, body in [('payload-1024.bin', (ROOT/'config/payload-1024.bin').read_bytes()), ('large.bin',large)]:
            channel.sendall(f'GET /{resource} HTTP/1.1\r\n'.encode())
            time.sleep(.002)
            channel.sendall(b'Host: 127.0.0.1\r\nConnection: keep-alive\r\n\r\n')
            if resource == 'large.bin':
                time.sleep(.02)
                channel.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024*1024)
            data = b''
            while b'\r\n\r\n' not in data:
                if time.monotonic() >= deadline: raise TimeoutError('header absolute deadline')
                part = channel.recv(4096)
                if not part: raise ValueError('EOF before response header')
                data += part
            header, received = data.split(b'\r\n\r\n',1)
            if not header.startswith(b'HTTP/1.1 200 '): raise ValueError('business status changed')
            while len(received) < len(body):
                if time.monotonic() >= deadline: raise TimeoutError('integration absolute deadline')
                part = channel.recv(65536)
                if not part: raise ValueError('short business body')
                received += part
            if received != body: raise ValueError('header/file body changed')
        channel.sendall(b'GET /pay')
        channel.shutdown(socket.SHUT_WR)
        while channel.recv(4096):
            if time.monotonic() >= deadline: raise TimeoutError('partial EOF deadline')
        channel.close(); channel=None
    except BaseException as failure:
        error = failure
    finally:
        actions = []
        if channel is not None: actions.append(('channel_close', channel.close))
        for operation, action in actions:
            try: action()
            except BaseException as failure:
                cleanup_errors.append({'operation': operation, 'type': type(failure).__name__, 'errno': getattr(failure,'errno',None)})
                if error is None: error = failure
        if owner is not None:
            try:
                cleanup = owner.close(float(os.environ['HP_BASELINE_CLEANUP_DEADLINE']))
                if cleanup['errors'] or cleanup['unknown'] or not cleanup['reaped'] or process.returncode != 0:
                    raise RuntimeError('integration cleanup incomplete or B export exit invalid')
            except BaseException as failure:
                cleanup_errors.append({'operation': 'child_cleanup', 'type': type(failure).__name__, 'errno': getattr(failure,'errno',None)})
                if error is None: error = failure
        for stream in streams:
            try: stream.close()
            except BaseException as failure:
                cleanup_errors.append({'operation': 'stream_close', 'type': type(failure).__name__, 'errno': getattr(failure,'errno',None)})
                if error is None: error = failure
        if error is not None:
            evidence = {'status':'invalid','first_error':{'type':type(error).__name__, 'errno':getattr(error,'errno',None), 'target_pid':process.pid if process else None, 'message':str(error)},
                        'cleanup_errors':cleanup_errors, 'cleanup':cleanup}
            try: (output/'failure.json').write_text(json.dumps(evidence)+'\n')
            except BaseException:
                try: sys.stderr.write(json.dumps(evidence)+'\n')
                except BaseException: pass
    if error is not None: raise error
    if b'connection write reached EAGAIN' not in (output/'server.stderr').read_bytes():
        raise ValueError('real EAGAIN continuation not exercised')
    metadata = json.loads((output/'boundary-worker-0.json').read_text())
    if metadata['invalid_reason'] or metadata['record_count'] != 3 or metadata['completed'] != 2 or metadata['incomplete'] != 1:
        raise ValueError('real Session/IO diagnostic changed')
    connection = metadata['connections'][0]
    if not connection['partial_eof'] or connection['tail_request_parsed'] or connection['last_sequence'] != 3:
        raise ValueError('real partial EOF state missing')
    with (output/'boundary-worker-0.bin').open('rb') as records:
        header=records.read(96)
        values=[struct.unpack('<IIQQQ', records.read(32)) for _ in range(3)]
        tail=records.read(96)
    if [row[1] for row in values] != [3,3,1] or [row[2] for row in values] != [1,2,3] or tail[8:] != header[8:]:
        raise ValueError('real read/drain continuity/export invalid')
    (output/'result.json').write_text(json.dumps({'status':'valid','fragmented_keepalive':2,
        'header_and_file_bytes':len(large),'partial_eof':True,'cleanup':cleanup,
        'scope':'real_B_IO_and_Session_not_performance_sample'})+'\n')

if __name__ == '__main__':
    main()
