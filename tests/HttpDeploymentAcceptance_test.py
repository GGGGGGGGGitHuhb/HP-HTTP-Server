"""Single CTest entry; real TCP close observations then the delivered deployment tool."""
import errno
import importlib.util
import json
import shutil
import subprocess
from unittest import mock
import os
from pathlib import Path
import socket
import struct
import sys
import threading
import uuid

from HttpChunkedExpectHttp_test import require_deadline_close


def tcp_pair():
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    client = peer = None
    try:
        listener.settimeout(5)
        listener.bind(('127.0.0.1', 0))
        listener.listen(1)
        client = socket.create_connection(listener.getsockname(), timeout=5)
        peer, _ = listener.accept()
        client.settimeout(5)
        peer.settimeout(5)
        return client, peer
    except BaseException:
        if client is not None:
            client.close()
        if peer is not None:
            peer.close()
        raise
    finally:
        listener.close()


def check_close_observation(reset, consumer):
    client, peer = tcp_pair()
    try:
        if reset:
            peer.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        peer.close()
        if consumer:
            observed = require_deadline_close(client)
        else:
            try:
                client.recv(1)
            except ConnectionResetError as error:
                if error.errno != errno.ECONNRESET:
                    raise
                observed = 'RST'
            else:
                raise AssertionError('abortive close did not produce actual ECONNRESET')
        expected = 'RST' if reset else 'EOF'
        if observed != expected:
            raise AssertionError('wrong close observation: ' + observed)
        return {'expected': expected, 'observed': observed, 'consumer': consumer}
    finally:
        client.close()
        peer.close()


def send_and_hold(peer, sent, release, errors):
    try:
        peer.sendall(b'x')
        sent.set()
        if not release.wait(5):
            raise TimeoutError('data peer release deadline')
    except BaseException as error:
        errors.append(repr(error))
        sent.set()


def check_data_rejection():
    client, peer = tcp_pair()
    sent, release = threading.Event(), threading.Event()
    errors = []
    sender = threading.Thread(target=send_and_hold, args=(peer, sent, release, errors))
    started = False
    try:
        sender.start()
        started = True
        if not sent.wait(5) or errors:
            raise AssertionError('real data not sent: ' + repr(errors))
        try:
            require_deadline_close(client)
        except AssertionError as error:
            if str(error) != 'deadline unexpectedly sent final/data':
                raise
        else:
            raise AssertionError('deadline consumer incorrectly accepted a data byte')
        return {'byte': 'x', 'rejected': True}
    finally:
        release.set()
        if started:
            sender.join(timeout=5)
        client.close()
        peer.close()
        if sender.is_alive() or errors:
            original = sys.exc_info()[1]
            message = 'data coordinator cleanup: ' + repr(errors)
            if original is not None:
                original.add_note(message)
            else:
                raise AssertionError(message)


def check_initialization(source, base, identity):
    fixture = base / ('deployment-initialization-' + identity)
    fixture.mkdir(mode=0o700)
    binary = fixture / 'execute-only'
    records = []
    try:
        shutil.copyfile('/usr/bin/true', binary)
        binary.chmod(0o111)
        artifacts = fixture / 'permission'
        result = subprocess.run([sys.executable, str(source / 'scripts/verify_deployment.py'),
                                 '--server', str(binary), '--artifacts', str(artifacts)],
                                capture_output=True, timeout=5)
        summary = json.loads((artifacts / 'summary.json').read_text())
        assert result.returncode == 1 and b'PermissionError' in result.stderr
        assert 'PermissionError' in summary['original_error'] and not summary['records']
        assert not (artifacts / 'work').exists() and binary.stat().st_mode & 0o777 == 0o111
        records.append({'case': 'real-mode0111-digest', 'exit': result.returncode,
                        'summary': summary, 'stderr': result.stderr.decode()})
        binary.chmod(0o700)
        spec = importlib.util.spec_from_file_location('deployment_initialization', source / 'scripts/verify_deployment.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        mkdir, write_text = Path.mkdir, Path.write_text
        for name in ('partial-mkdir', 'summary-failure'):
            artifacts = fixture / name
            original = OSError('injected root mkdir')
            reached = []
            def fail_root(path, *args, **kwargs):
                if path == artifacts / 'work/www':
                    assert (artifacts / 'work').is_dir()
                    reached.append(True)
                    raise original
                return mkdir(path, *args, **kwargs)
            def fail_summary(path, *args, **kwargs):
                if name == 'summary-failure' and path == artifacts / 'summary.json':
                    raise OSError('injected summary write')
                return write_text(path, *args, **kwargs)
            with mock.patch.object(Path, 'mkdir', fail_root), mock.patch.object(Path, 'write_text', fail_summary):
                try:
                    module.run(binary, artifacts, False)
                except OSError as error:
                    assert error is original
                else:
                    raise AssertionError('initialization fault did not escape')
            assert reached == [True] and not (artifacts / 'work').exists() and binary.exists()
            if name == 'partial-mkdir':
                summary = json.loads((artifacts / 'summary.json').read_text())
                assert summary['root_removed'] and not summary['passed'] and not summary['records']
            else:
                assert any('injected summary write' in note for note in original.__notes__)
            records.append({'case': name, 'original_preserved': True, 'work_removed': True})
        # Rejected pre-existing output belongs to the caller and stays untouched.
        marker = fixture / 'keep'; marker.write_text('caller-owned')
        try:
            module.run(binary, fixture, False)
        except AssertionError:
            pass
        else:
            raise AssertionError('existing artifacts accepted')
        assert marker.read_text() == 'caller-owned'
    finally:
        if binary.exists():
            binary.chmod(0o700)
        shutil.rmtree(fixture)
    record = base / ('deployment-initialization-' + identity + '.json')
    record.write_text(json.dumps({'passed': True, 'records': records, 'fixture_removed': not fixture.exists()}, indent=2))
    print('INITIALIZATION_SUMMARY', record, flush=True)


source = Path(__file__).resolve().parents[1]
base = Path(os.environ['HP_S3_TEST_TMP_ROOT']).resolve()
base.mkdir(parents=True, exist_ok=True)
identity = uuid.uuid4().hex
check_initialization(source, base, identity)
records = [check_close_observation(False, True), check_close_observation(True, False),
           check_close_observation(True, True), check_data_rejection()]
transport = base / ('deployment-transport-' + identity + '.json')
transport.write_text(json.dumps({'passed': True, 'records': records,
                                 'sockets_closed': True, 'coordinator_joined': True}, indent=2))
print('TRANSPORT_SUMMARY', transport, flush=True)
artifacts = base / ('deployment-evidence-' + identity)
os.execv(sys.executable, [sys.executable, str(source / 'scripts/verify_deployment.py'),
                         '--server', str(Path(sys.argv[1]).resolve()), '--artifacts', str(artifacts)])
