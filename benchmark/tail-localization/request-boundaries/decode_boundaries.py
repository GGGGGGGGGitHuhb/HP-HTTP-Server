"""Builder独立流式关联R018完整原料，不将区间当作根因。"""
import argparse
from fractions import Fraction
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
sys.dont_write_bytecode = True
ROOT = Path(__file__).absolute().parent
spec = importlib.util.spec_from_file_location('own_baseline_stats', ROOT / 'verify_baseline_stats.py')
stats = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stats)


def require(value, reason):
    if not value:
        raise ValueError(reason)


def read_json(path):
    return json.loads(stats.read_regular(path, 16 * 1024 * 1024))


def tuple_key(row):
    fields = ('client_address_u32', 'client_port', 'server_address_u32', 'server_port')
    values = tuple(row[key] for key in fields)
    require(all(type(value) is int for value in values), 'tuple integer types')
    require(values[0] == values[2] == 0x7f000001 and 0 < values[1] <= 65535 and 0 < values[3] <= 65535, 'IPv4 tuple direction/range')
    return values


def verify_clocks(metadata, process):
    captured = process['clock_identity']
    start, stop = metadata['clock_start'], metadata['clock_stop']
    for row in (start, stop):
        require(row['clock'] == 'CLOCK_MONOTONIC' and row['resolution_ns'] > 0 and
                row['process_starttime'] == process['identity']['starttime'] and
                (row['boot_id'], row['time_namespace']) == (captured['boot_id'], captured['time_namespace']), 'actual start/stop clock identity')
    require(0 < start['observed_ns'] <= stop['observed_ns'], 'clock lifecycle order')


def decode_sample(directory, verify_statistics=True, sample_override=None):
    directory = Path(directory)
    if verify_statistics:
        base = stats.verify_sample(directory)
    else:
        base = None
    sample = sample_override if sample_override is not None else read_json(directory / 'sample.json')
    client = read_json(directory / 'client.json')
    mapping = read_json(directory / 'client-map.json')
    processes = {row['name']: row for row in sample['processes']}
    client_identity = processes['client']['identity']
    verify_clocks(mapping, processes['client'])
    require(mapping['schema'] == 'baseline-v1-map' and mapping['run_id'] == sample['run_id'] and
            mapping['pid'] == client_identity['pid'] and mapping['process_starttime'] == client_identity['starttime'], 'map process identity')
    clocks = [processes[name]['clock_identity'] for name in ('server', 'client')]
    require(all(row['clock'] == 'CLOCK_MONOTONIC' and row['clock_resolution_ns'] > 0 for row in clocks) and
            (clocks[0]['boot_id'], clocks[0]['time_namespace']) == (clocks[1]['boot_id'], clocks[1]['time_namespace']), 'cross-process clock identity')
    clients = {(owner['owner'], row['life']): row for owner in client['threads'] for row in owner['connections']}
    require(len(clients) == 128 and len(mapping['connections']) == 128, '128 client life coverage')
    mapped = {}
    for row in mapping['connections']:
        key = (row['owner'], row['life'])
        require(key in clients and key not in mapped and row['final_sequence'] == clients[key]['sequence'] and
                row['connected_ns'] == clients[key]['ready_ns'] and row['connected_ns'] < client['warm_start_ns'], 'map lifetime/final sequence')
        mapped[key] = row
    require(len({tuple_key(row) for row in mapped.values()}) == 128, 'duplicate client tuple')
    if sample['mode'] == 'O':
        require(not any(path.name.startswith('boundary-worker-') for path in directory.iterdir()), 'O has observer output')
        return {'run_id': sample['run_id'], 'mode': 'O', 'statistics': base, 'records': []}
    server_identity = processes['server']['identity']
    tables = {}
    by_tuple = {}
    for worker in range(4):
        metadata = read_json(directory / f'boundary-worker-{worker}.json')
        require(metadata['schema'] == 'request-boundaries-worker-v1', 'worker schema')
        verify_clocks(metadata, processes['server'])
        require(metadata['worker'] == worker and metadata['pid'] == server_identity['pid'] and metadata['process_starttime'] == server_identity['starttime'] and
                not metadata['invalid_reason'] and metadata['writer_stopped'] and not metadata['overflow'], 'worker identity/status')
        require(len(metadata['connections']) == 32, 'worker roundrobin32')
        for connection in metadata['connections']:
            identity = (worker, connection['connection_id'])
            key = tuple_key(connection)
            require(identity not in tables and key not in by_tuple and 0 < connection['opened_ns'] <= connection['closed_ns'], 'server tuple/life uniqueness')
            tables[identity] = connection
            by_tuple[key] = identity
    require(len(tables) == 128 and set(by_tuple) == {tuple_key(row) for row in mapped.values()}, '128 cross-end bijection')
    client_for_server = {by_tuple[tuple_key(row)]: key for key, row in mapped.items()}
    wanted = {}
    for owner in client['threads']:
        for row in owner['slow_records']:
            key = (owner['owner'], row['life'], row['sequence'])
            require(key not in wanted and row['class'] in (1, 2) and row['status'] == 200 and row['body_bytes'] == 1024, 'slow identity/response')
            wanted[key] = row
    matches = {}
    final = {key: {'sequence': 0, 'completed': 0, 'incomplete': 0} for key in tables}
    for worker in range(4):
        metadata = read_json(directory / f'boundary-worker-{worker}.json')
        path = directory / f'boundary-worker-{worker}.bin'
        for parent in (path, *path.parents):
            require(not parent.is_symlink(), 'wire symlink')
        with path.open('rb') as stream:
            header = stream.read(96)
            require(len(header) == 96, 'missing header')
            fields = struct.unpack('<8sII10Q', header)
            require(fields[:3] == (b'HPBOUND1', 1, worker) and fields[3:6] == (metadata['pid'], metadata['tid'], metadata['process_starttime']) and
                    fields[9:13] == (0, 1, 1, 0), 'wire header status/identity')
            count = fields[6]
            require(count <= 524288 and fields[6:9] == (metadata['record_count'], metadata['completed'], metadata['incomplete']) and fields[7] + fields[8] == count, 'wire capacity/count')
            completed = incomplete = 0
            for index in range(count):
                value = stream.read(32)
                require(len(value) == 32, 'partial record')
                connection_id, flags, sequence, read_ns, drained_ns = struct.unpack('<IIQQQ', value)
                key = (worker, connection_id)
                require(key in tables and flags in (1, 3), 'unknown connection/record flags')
                connection, previous = tables[key], final[key]
                require(sequence == previous['sequence'] + 1 and not previous['incomplete'] and
                        connection['opened_ns'] <= read_ns <= connection['closed_ns'], 'server request continuity/life clock')
                require((flags == 1 and drained_ns == 0) or (flags == 3 and read_ns <= drained_ns <= connection['closed_ns']), 'R/D state/clock')
                previous['sequence'] = sequence
                previous['completed' if flags == 3 else 'incomplete'] += 1
                completed += flags == 3
                incomplete += flags == 1
                owner_life = client_for_server[key]
                request_key = (*owner_life, sequence)
                if request_key in wanted:
                    require(flags == 3 and request_key not in matches, 'slow incomplete/duplicate')
                    record = wanted[request_key]
                    c0, c1 = record['start_ns'], record['complete_ns']
                    require(client['warm_start_ns'] <= c0 <= read_ns and client['T0_ns'] <= c1 < client['T1_ns'], 'client/server signed identity violation')
                    ordered = c0 <= read_ns <= drained_ns <= c1
                    differences = [read_ns - c0, drained_ns - read_ns, c1 - drained_ns]
                    intersection = [max(c0, read_ns), min(c1, drained_ns)]
                    if intersection[1] < intersection[0]: intersection[1] = intersection[0]
                    matches[request_key] = {'client_owner': owner_life[0], 'client_life': owner_life[1], 'server_worker': worker,
                        'server_connection_id': connection_id, 'sequence': sequence, 'tuple': list(tuple_key(connection)),
                        'C0_ns': c0, 'R_ns': read_ns, 'D_ns': drained_ns, 'C1_ns': c1, 'raw_ns': c1 - c0,
                        'signed_intervals_ns': differences, 'ordered': ordered, 'intersection_ns': intersection,
                        'longest_interval': differences.index(max(differences)) if ordered else None,
                        'completion_bucket_5ms': (c1 - client['T0_ns']) // 5000000}
            trailer = stream.read(96)
            require(len(trailer) == 96 and trailer[:8] == b'HPBTAIL1' and trailer[8:] == header[8:] and not stream.read(1), 'wire trailer/extra bytes')
            require((completed, incomplete) == fields[7:9], 'actual record count vs header')
    totals = [0, 0]
    for server_key, connection in tables.items():
        key = client_for_server[server_key]
        state = clients[key]
        count = final[server_key]
        require(count['sequence'] == connection['last_sequence'], 'metadata last sequence')
        sequence = state['sequence']
        if not state['active']:
            require(count['sequence'] == sequence and not count['incomplete'], 'completed final request missing')
        elif not state['full_sent']:
            require(state['end_reason'] == 1 and 0 <= state['sent_bytes'] < sample['request_bytes'], 'partial terminal evidence')
            if state['sent_bytes'] == 0:
                require(count['sequence'] == sequence - 1 and not count['incomplete'], 'unsent cannot have R')
            else:
                require((count['sequence'] == sequence - 1 and not count['incomplete']) or
                        (count['sequence'] == sequence and count['incomplete'] == 1 and connection['partial_eof'] and not connection['tail_request_parsed']), 'partial EOF must be exact last slot')
        else:
            require(state['end_reason'] == 4 and count['sequence'] in (sequence - 1, sequence), 'sent deadline terminal evidence')
        totals[key[0]] += sequence - int(state['active'])
    require(totals == [sum(owner['completed']) for owner in client['threads']], 'all client completed sequence accounting')
    require(len(matches) == len(wanted), 'all completed slow records missing boundary')
    rows = sorted(matches.values(), key=lambda row: (row['C1_ns'], row['C0_ns'], row['client_owner'], row['client_life'], row['sequence']))
    buckets = {}
    for row in rows:
        bucket = buckets.setdefault(row['completion_bucket_5ms'], {'index': row['completion_bucket_5ms'], 'count': 0,
            'ordered_count': 0, 'crossed_count': 0, 'longest_interval_counts': [0, 0, 0], 'request_keys': []})
        bucket['count'] += 1
        bucket['ordered_count' if row['ordered'] else 'crossed_count'] += 1
        if row['ordered']: bucket['longest_interval_counts'][row['longest_interval']] += 1
        bucket['request_keys'].append([row['client_owner'], row['client_life'], row['sequence']])
    return {'run_id': sample['run_id'], 'mode': 'B', 'statistics': base, 'records': rows,
            'connection_coverage': [{'worker': worker, 'connection_id': life, **final[(worker, life)]} for worker, life in sorted(tables)],
            'buckets_5ms': [buckets[index] for index in sorted(buckets)],
            'slow_count': len(rows), 'ordered_count': sum(row['ordered'] for row in rows), 'crossed_count': sum(not row['ordered'] for row in rows),
            'top10': sorted(rows, key=lambda row: (-row['raw_ns'], row['client_owner'], row['client_life'], row['sequence']))[:10]}


def ratio(value):
    return {'numerator': value.numerator, 'denominator': value.denominator}


def compare(samples):
    o1, b, o2 = [row['statistics'] for row in samples]
    warnings = []
    comparisons = {}
    for field, threshold in [('N', Fraction(15,100)), ('main_raw_p99_us', Fraction(25,100)), ('corrected_p99_us', Fraction(25,100))]:
        left, middle, right = (row[field] for row in (o1, b, o2))
        if min(left, right) <= 0:
            comparisons[field] = {'unavailable': 'nonpositive denominator'}
            warnings.append(field + ':unavailable')
            continue
        spread = Fraction(max(left,right)-min(left,right), min(left,right))
        difference = Fraction(abs(2*middle-left-right), left+right)
        comparisons[field] = {'O_spread': ratio(spread), 'B_vs_O_median': ratio(difference)}
        if spread > threshold: warnings.append(field + ':O_unstable')
        if difference > threshold: warnings.append(field + ':group_difference')
    slow = [Fraction(row['slow_count'], row['N']) for row in (o1,b,o2)]
    if min(slow[0],slow[2]) == 0:
        warnings.append('slow_frequency:unavailable')
        comparisons['slow_frequency'] = {'unavailable': 'zero O denominator'}
    else:
        spread = max(slow[0],slow[2])/min(slow[0],slow[2])
        difference = slow[1]/((slow[0]+slow[2])/2)
        comparisons['slow_frequency'] = {'O_ratio': ratio(spread), 'B_vs_O_median': ratio(difference)}
        if spread > 2 or not Fraction(1,2) <= difference <= 2: warnings.append('slow_frequency:unstable')
    return {'comparisons': comparisons, 'warnings': warnings, 'perturbation_separated': not warnings,
            'scope': 'overall_observer_difference_or_natural_variation_not_separated_if_warning'}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--role', choices=('builder','reviewer'), required=True)
    parser.add_argument('--output-root', required=True)
    parser.add_argument('--build-output', required=True)
    args = parser.parse_args()
    stage = Path(args.output_root).parents[1]
    require(Path(args.output_root) == stage / args.role / 'run-r018-decode-001', 'decode exact route')
    paths = [stage / 'reviewer' / f'run-r018-boundary-{index:02}' for index in range(1,4)]
    smoke = decode_sample(stage / args.role / 'run-r018-smoke-001')
    samples = [decode_sample(path) for path in paths]
    result = {'schema': 'request-boundaries-analysis-v1', 'status': 'valid', 'smoke': smoke, 'samples': samples,
              'control': compare(samples), 'cause_confirmed': False,
              'core_observation': 'PASS' if samples[1]['slow_count'] and compare(samples)['perturbation_separated'] else 'BLOCKED'}
    (Path(args.output_root) / 'analysis.json').write_text(json.dumps(result, indent=2, allow_nan=False)+'\n')

if __name__ == '__main__':
    main()
