#!/usr/bin/env python3
"""Run one authorized smoke or the fixed 18-sample matrix; never retry."""
import argparse
import hashlib
import json
import math
import os
import pathlib
import platform
import re
import socket
import statistics
import time

from Build import validate_manifest
from Common import REPO, WRK, LIB, Budget, OwnedProcess, demand, fresh_directory, process_info, role_root, save, sha

MATRIX = {
    'M1': {'size': 1024, 'mode': 'keepalive', 'workers': 0, 'threads': 1, 'connections': 1},
    'M2': {'size': 1024, 'mode': 'keepalive', 'workers': 2, 'threads': 2, 'connections': 32},
    'M3': {'size': 1024, 'mode': 'keepalive', 'workers': 4, 'threads': 4, 'connections': 128},
    'M4': {'size': 1024, 'mode': 'short', 'workers': 2, 'threads': 2, 'connections': 32},
    'M5': {'size': 65536, 'mode': 'keepalive', 'workers': 2, 'threads': 2, 'connections': 32},
    'M6': {'size': 1048576, 'mode': 'keepalive', 'workers': 2, 'threads': 2, 'connections': 32},
}
SEQUENCE = ['M1', 'M2', 'M3', 'M4', 'M5', 'M6', 'M6', 'M5', 'M4', 'M3', 'M2', 'M1', 'M3', 'M4', 'M5', 'M6', 'M1', 'M2']
LUA = pathlib.Path(__file__).with_name('Summary.lua')


def unique_pairs(pairs):
    result = {}
    for key, value in pairs:
        demand(key not in result, 'duplicate JSON field')
        result[key] = value
    return result


def parse_summary(text, returncode, payload_size, connections):
    demand(returncode == 0, 'wrk nonzero exit')
    lines = [line[len('MATRIX_SUMMARY '):] for line in text.splitlines() if line.startswith('MATRIX_SUMMARY ')]
    demand(len(lines) == 1, 'missing or duplicate summary')
    data = json.loads(lines[0], object_pairs_hook=unique_pairs)
    demand(data['schema'] == 2, 'wrong summary schema')
    for key in ('duration_us', 'requests', 'bytes'):
        value = data[key]
        demand(type(value) in (int, float) and math.isfinite(value) and value > 0 and int(value) == value, 'invalid count ' + key)
    for key in ('connect', 'read', 'write', 'status', 'timeout'):
        demand(type(data['errors'][key]) in (int, float) and data['errors'][key] == 0, 'measurement errors: ' + key)
    for value in data['latency_us'].values():
        demand(type(value) in (int, float) and math.isfinite(value) and value >= 0, 'nonfinite/negative latency')
    latency = data['latency_us']
    demand(latency['p50'] <= latency['p95'] <= latency['p99'] <= latency['max'] and latency['mean'] <= latency['max'], 'latency ordering')
    interval = data['correction_interval_us']
    expected = data['duration_us'] * connections / data['requests']
    demand(type(interval) in (int, float) and math.isfinite(interval) and abs(interval - expected) <= 1e-5, 'correction interval mismatch')
    demand(data['latency_distribution'] == 'wrk_corrected' and data['population_status'] == 'not_collected', 'population metadata mismatch')
    demand(data['corrected_population'] is None and data['nonzero_bins'] is None, 'population must be explicit null')
    demand(data['bytes'] >= data['requests'] * payload_size, 'body accounting shortfall')
    seconds = data['duration_us'] / 1e6
    return {**data, 'qps': data['requests'] / seconds, 'received_mib_s': data['bytes'] / seconds / 1048576}


def parse_metrics(text):
    demand(text.count('HP_METRICS_BEGIN') == text.count('HP_METRICS_END') == 1, 'missing/duplicate metrics marker')
    data = {}
    for line in text.split('HP_METRICS_BEGIN\n')[1].split('HP_METRICS_END')[0].splitlines():
        key, value = line.split()
        demand(key not in data, 'duplicate metric')
        data[key] = int(value)
        demand(data[key] >= 0, 'negative metric')
    demand(data['connections_active'] == data['logger_pending'] == 0, 'final active/logs not zero')
    demand(data['requests_started_total'] == data['responses_completed_total'] + data['requests_aborted_total'] == data['latency_count'], 'S1 final accounting')
    return data


def listener_owned(child, port):
    demand(child.matches(), 'server PID identity changed')
    inodes = set()
    for fd in (pathlib.Path('/proc') / str(child.process.pid) / 'fd').iterdir():
        try:
            target = os.readlink(fd)
        except FileNotFoundError:
            continue
        if target.startswith('socket:['):
            inodes.add(target[8:-1])
    for line in pathlib.Path('/proc/net/tcp').read_text().splitlines()[1:]:
        fields = line.split()
        if int(fields[1].split(':')[1], 16) == port and fields[3] == '0A' and fields[9] in inodes:
            return True
    return False


def ready(child, budget):
    deadline = min(time.monotonic() + 5, budget.phase_deadline)
    while child.poll() is None:
        budget.check(deadline)
        match = re.search(r'listening on port (\d+)\.', child.stdout_path.read_text())
        if match:
            port = int(match.group(1))
            demand(listener_owned(child, port), 'port not owned by server')
            return port
        time.sleep(0.01)
    raise ValueError('server exited before readiness')


def audit(port, payload, mode, budget):
    size, digest, name = payload['size'], payload['sha256'], payload['name']
    audits = []
    peer = None
    try:
        for index in range(3):
            budget.check()
            if peer is None:
                peer = socket.create_connection(('127.0.0.1', port), timeout=min(2, budget.phase_deadline - time.monotonic()))
            peer.settimeout(min(2, budget.phase_deadline - time.monotonic()))
            peer.sendall(f'GET /{name} HTTP/1.1\r\nHost: loopback\r\nConnection: {"close" if mode == "short" else "keep-alive"}\r\n\r\n'.encode())
            received = b''
            while b'\r\n\r\n' not in received:
                part = peer.recv(65536)
                demand(part, 'premature audit header EOF')
                received += part
                budget.check()
            header, body = received.split(b'\r\n\r\n', 1)
            demand(header.startswith(b'HTTP/1.1 200 '), 'audit status is not 200')
            fields = dict(line.split(b':', 1) for line in header.split(b'\r\n')[1:])
            demand(int(fields[b'Content-Length']) == size, 'audit content length')
            demand(fields[b'Connection'].strip() == (b'close' if mode == 'short' else b'keep-alive'), 'connection policy mismatch')
            while len(body) < size:
                part = peer.recv(65536)
                demand(part, 'premature audit body EOF')
                body += part
                budget.check()
            demand(len(body) == size and hashlib.sha256(body).hexdigest() == digest, 'audit body hash/extra bytes')
            if mode == 'short':
                demand(peer.recv(1) == b'', 'short connection did not EOF')
                peer.close()
                peer = None
            else:
                peer.settimeout(0.01)
                try:
                    tail = peer.recv(1)
                    raise ValueError('unexpected keepalive tail/EOF: ' + repr(tail))
                except socket.timeout:
                    pass
            audits.append({'request': index + 1, 'status': 200, 'body_bytes': size, 'sha256': digest, 'connection': index + 1 if mode == 'short' else 1, 'eof': mode == 'short', 'extra_bytes': 0})
    finally:
        if peer:
            peer.close()
    return audits


def validate_cpu_evidence(measurement, server_identity, client_identity):
    evidence = measurement['cpu_evidence']
    demand(evidence['schema'] == 1 and evidence['units'] == {'ticks': 'clock ticks', 'time': 'seconds', 'cpu_pct': 'one core = 100%'}, 'CPU evidence schema/units')
    def finite(value, name, positive=False):
        demand(type(value) in (int, float) and math.isfinite(value) and (value > 0 if positive else value >= 0), 'invalid CPU ' + name)
        return value
    start = finite(measurement['started_monotonic'], 'start')
    end = finite(measurement['ended_monotonic'], 'end')
    wall = finite(measurement['wall_s'], 'wall', True)
    demand(end > start and math.isclose(wall, end - start, rel_tol=1e-9, abs_tol=1e-9), 'CPU clock wall mismatch')
    before, after = evidence['server_before'], evidence['server_after']
    for snapshot in (before, after):
        demand(snapshot['pid'] == server_identity['pid'] and snapshot['starttime'] == server_identity['starttime'], 'CPU server identity mismatch')
        for key in ('pid', 'starttime', 'utime_ticks', 'stime_ticks', 'clock_ticks_per_second'):
            demand(type(snapshot[key]) is int and snapshot[key] >= (0 if key.endswith('_ticks') else 1), 'invalid CPU ' + key)
        finite(snapshot['read_monotonic'], 'snapshot clock')
        seconds = finite(snapshot['cpu_seconds'], 'seconds')
        demand(math.isclose(seconds, (snapshot['utime_ticks'] + snapshot['stime_ticks']) / snapshot['clock_ticks_per_second'], rel_tol=1e-9, abs_tol=1e-9), 'CPU seconds conversion mismatch')
    demand(before['clock_ticks_per_second'] == after['clock_ticks_per_second'], 'CPU tick frequency mismatch')
    demand(start <= before['read_monotonic'] <= after['read_monotonic'] <= end, 'CPU snapshot clock ordering')
    delta = (after['utime_ticks'] + after['stime_ticks'] - before['utime_ticks'] - before['stime_ticks']) / before['clock_ticks_per_second']
    finite(delta, 'delta')
    client = evidence['client']
    demand(client['pid'] == client_identity['pid'] and client['starttime'] == client_identity['starttime'] and client['wait4_pid'] == client['pid'], 'CPU client/wait4 identity mismatch')
    for key in ('pid', 'starttime', 'wait4_pid'):
        demand(type(client[key]) is int and client[key] > 0, 'invalid CPU client identity')
    usage = finite(client['ru_utime_s'], 'user seconds') + finite(client['ru_stime_s'], 'system seconds')
    finite(client['wait4_read_monotonic'], 'wait4 clock')
    demand(start <= client['wait4_read_monotonic'] <= end, 'CPU wait4 clock ordering')
    for name, expected in (('server_cpu_pct', 100 * delta / wall), ('client_cpu_pct', 100 * usage / wall)):
        value = finite(measurement[name], name)
        demand(math.isclose(value, expected, rel_tol=1e-9, abs_tol=1e-9), 'CPU derived percentage mismatch')
    return {'server_cpu_seconds_delta': delta, 'client_cpu_seconds': usage}


def cpu_evidence(before, after, child):
    demand(child.usage is not None, 'missing CPU wait4 usage')
    return {'schema': 1, 'units': {'ticks': 'clock ticks', 'time': 'seconds', 'cpu_pct': 'one core = 100%'},
            'server_before': before, 'server_after': after,
            'client': {'pid': child.identity['pid'], 'starttime': child.identity['starttime'],
                       'wait4_pid': child.wait4_pid, 'ru_utime_s': child.usage.ru_utime,
                       'ru_stime_s': child.usage.ru_stime, 'wait4_read_monotonic': child.wait4_read_monotonic}}


def wrk_phase(server, port, payload, config, seconds, prefix, budget):
    env = os.environ.copy()
    env.update(LD_LIBRARY_PATH=str(LIB), HP_MATRIX_MODE=config['mode'], HP_MATRIX_CONNECTIONS=str(config['connections']), NO_PROXY='127.0.0.1,localhost', no_proxy='127.0.0.1,localhost')
    argv = [str(WRK), '-t', str(config['threads']), '-c', str(config['connections']), '--timeout', '2s', '--latency', '-d', str(seconds) + 's', '-s', str(LUA), f'http://127.0.0.1:{port}/{payload["name"]}']
    started, wall_started = time.monotonic(), time.time()
    before = process_info(server.process.pid)
    child = OwnedProcess(argv, prefix, env)
    rss_max, samples, next_sample = before['rss_kib'], [], started
    try:
        while child.poll() is None:
            budget.check(min(started + seconds + 5, budget.phase_deadline))
            demand(server.poll() is None and server.matches(), 'server failed during load')
            now = time.monotonic()
            if now >= next_sample:
                observed = process_info(server.process.pid)
                try:
                    client = process_info(child.process.pid)
                except FileNotFoundError:
                    continue  # wait4在下次循环读取真实退出与rusage，不伪造RSS零值。
                samples.append({'elapsed_s': now - started, 'server_rss_kib': observed['rss_kib'], 'client_rss_kib': client['rss_kib']})
                rss_max = max(rss_max, observed['rss_kib'])
                next_sample = now + 1
            time.sleep(0.05)
        after = process_info(server.process.pid)
        cleanup = child.close(budget.data['deadline_monotonic'])
        ended = time.monotonic()
        result = parse_summary(child.stdout_path.read_text(), cleanup['returncode'], payload['size'], config['connections'])
        measurement = {'summary': result, 'argv': argv, 'environment': {'HP_MATRIX_MODE': config['mode'], 'HP_MATRIX_CONNECTIONS': str(config['connections']), 'LD_LIBRARY_PATH': str(LIB)},
                'started_monotonic': started, 'ended_monotonic': ended, 'started_wall_s': wall_started, 'ended_wall_s': time.time(),
                'wall_s': ended - started, 'server_cpu_pct': 100 * (after['cpu_seconds'] - before['cpu_seconds']) / (ended - started),
                'client_cpu_pct': 100 * (child.usage.ru_utime + child.usage.ru_stime) / (ended - started),
                'server_rss_sampled_max_kib': rss_max, 'client_rss_sampled_max_kib': max(s['client_rss_kib'] for s in samples),
                'server_vmhwm_kib': after['hwm_kib'], 'rss_samples': samples, 'cleanup': cleanup, 'cpu_evidence': cpu_evidence(before, after, child)}
        validate_cpu_evidence(measurement, server.identity, child.identity)
        return measurement
    finally:
        if child.process.returncode is None:
            child.close(budget.data['deadline_monotonic'])


def sample(manifest, config, directory, payload_root, payload, budget, warmup, duration):
    started, wall_started = time.monotonic(), time.time()
    row = {'status': 'running', 'config': config, 'payload': payload, 'started_monotonic': started, 'started_wall_s': wall_started}
    server = None
    try:
        argv = [manifest['binary'], '--port', '0', '--root', str(payload_root), '--threads', str(config['workers']), '--idle-timeout-ms', '30000', '--keep-alive-timeout-ms', '15000', '--shutdown-timeout-ms', '5000', '--metrics-on-exit']
        server = OwnedProcess(argv, directory / 'server')
        row['server_argv'] = argv
        port = ready(server, budget)
        row['port'] = port
        row['pre_audit'] = audit(port, payload, config['mode'], budget)
        row['warmup'] = wrk_phase(server, port, payload, config, warmup, directory / 'warmup', budget)
        row['measurement'] = wrk_phase(server, port, payload, config, duration, directory / 'measurement', budget)
        row['post_audit'] = audit(port, payload, config['mode'], budget)
        row['status'] = 'valid'
    except BaseException as error:
        row.update(status='invalid', error=repr(error))
        raise
    finally:
        try:
            if server:
                row['server_cleanup'] = server.close(budget.data['deadline_monotonic'])
                demand(not row['server_cleanup']['forced'] and row['server_cleanup']['returncode'] == 0, 'server cleanup invalid')
                row['metrics'] = parse_metrics(server.stdout_path.read_text())
                if 'port' in row:
                    with socket.socket() as probe:
                        probe.settimeout(0.1)
                        demand(probe.connect_ex(('127.0.0.1', row['port'])) != 0, 'owned port still listening after reap')
            budget.check()
        except BaseException as error:
            row.update(status='invalid', cleanup_error=repr(error))
        row.update(ended_monotonic=time.monotonic(), ended_wall_s=time.time())
        row['complete_wall_s'] = row['ended_monotonic'] - started
        save(directory / 'result.json', row)
    demand(row['status'] == 'valid', 'sample cleanup/final accounting invalid')
    return row


def aggregate(rows):
    result = {}
    for identifier in MATRIX:
        selected = [row['measurement'] for row in rows if row['id'] == identifier]
        demand(len(selected) == 3, 'matrix missing repeats')
        fields = {'qps': [r['summary']['qps'] for r in selected], 'received_mib_s': [r['summary']['received_mib_s'] for r in selected],
                  'server_cpu_pct': [r['server_cpu_pct'] for r in selected], 'client_cpu_pct': [r['client_cpu_pct'] for r in selected],
                  'server_rss_sampled_max_kib': [r['server_rss_sampled_max_kib'] for r in selected]}
        for name in ('mean', 'p50', 'p95', 'p99', 'max'):
            fields['latency_' + name + '_us'] = [r['summary']['latency_us'][name] for r in selected]
        result[identifier] = {}
        for name, values in fields.items():
            median = statistics.median(values)
            result[identifier][name] = {'values': values, 'median': median, 'min': min(values), 'max': max(values), 'span_over_median': (max(values) - min(values)) / median if median else None}
    return result


def sample_accounting(sequence, samples):
    started = len(samples)
    demand([row['index'] for row in samples] == list(range(1, started + 1)), 'sample index mismatch')
    remaining = [{'id': identifier, 'index': index + 1, 'repeat': index // 6 + 1, 'status': 'not_run'}
                 for index, identifier in enumerate(sequence) if index >= started]
    return {'valid': sum(row['status'] == 'valid' for row in samples),
            'invalid': sum(row['status'] == 'invalid' for row in samples), 'not_run': len(remaining)}, remaining


def run(root, mode, recovery=False):
    budget = Budget(root, recovery=recovery)
    budget.begin(mode)
    output = None
    result = {'schema': 2, 'mode': mode, 'status': 'running', 'samples': [], 'sequence': ['M3' if recovery else 'M2'] if mode == 'smoke' else SEQUENCE,
              'environment': {'kernel': platform.release(), 'machine': platform.machine(), 'os': platform.system(), 'cpu_count': os.cpu_count(), 'transport': 'same-host WSL2 loopback', 'cache': 'hot', 'load': 'closed-loop'}}
    try:
        original = json.loads((root / 'budget.json').read_text())
        demand(original['phases']['build']['status'] == budget.data['phases']['fastchecks']['status'] == 'passed', 'build/fastchecks prerequisite not passed')
        if mode == 'formal':
            demand(budget.data['phases']['smoke']['status'] == 'passed', 'smoke not passed')
        manifest = validate_manifest(root / 'artifact/manifest.json')
        output = fresh_directory(budget.output_root / ('suite-' + mode))
        payload_root = fresh_directory(output / 'payload')
        payloads = {}
        for size in sorted({config['size'] for config in MATRIX.values()}):
            name = f'payload-{size}.bin'
            (payload_root / name).write_bytes(bytes(range(256)) * (size // 256))
            payloads[size] = {'name': name, 'size': size, 'sha256': sha(payload_root / name)}
        result['identity'] = {key: manifest[key] for key in ('commit', 'tree', 'binary_sha256', 'compiler', 'compiler_sha256', 'cmake', 'wrk', 'server_libraries')}
        result['tool_hashes'] = {p.name: sha(p) for p in pathlib.Path(__file__).parent.glob('*') if p.is_file() and p.suffix in ('.py', '.lua')}
        save(output / 'result.json', result)
        for index, identifier in enumerate(result['sequence']):
            budget.check()
            directory = fresh_directory(output / f'{index + 1:02}-{identifier}')
            identity = {'id': identifier, 'index': index + 1, 'repeat': index // 6 + 1 if mode == 'formal' else 0}
            try:
                row = sample(manifest, MATRIX[identifier], directory, payload_root, payloads[MATRIX[identifier]['size']], budget, 1 if mode == 'smoke' else 2, 1 if mode == 'smoke' else 10)
            except BaseException:
                row = json.loads((directory / 'result.json').read_text()) if (directory / 'result.json').exists() else {'status': 'invalid'}
                row.update(identity)
                result['samples'].append(row)
                save(directory / 'result.json', row)
                raise
            row.update(identity)
            result['samples'].append(row)
            save(directory / 'result.json', row)
            save(output / 'result.json', result)
            print(f'{mode} {index + 1}/{len(result["sequence"])} {identifier}: valid', flush=True)
        validate_manifest(root / 'artifact/manifest.json')
        demand(result['tool_hashes'] == {p.name: sha(p) for p in pathlib.Path(__file__).parent.glob('*') if p.is_file() and p.suffix in ('.py', '.lua')}, 'matrix tool changed during suite')
        if mode == 'formal':
            result['groups'] = aggregate(result['samples'])
        result['status'] = 'valid'
        budget.finish('passed')
    except BaseException as error:
        result.update(status='invalid', error=repr(error))
        budget.finish('invalid', repr(error))
        raise
    finally:
        result['counts'], result['not_run'] = sample_accounting(result['sequence'], result['samples'])
        save((output or budget.output_root) / ('result.json' if output else mode + '-failure.json'), result)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True)
    parser.add_argument('--mode', choices=('smoke', 'formal'), required=True)
    parser.add_argument('--recovery', action='store_true')
    parser.add_argument('--recovery-name', choices=('rework-001', 'rework-002'))
    args = parser.parse_args(argv)
    run(role_root(args.root), args.mode, args.recovery_name or args.recovery)


if __name__ == '__main__':
    main()
