"""D diagnostic raw evidence validation; correlations never assert causality."""
import argparse
import collections
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
sys.dont_write_bytecode = True
ROOT = Path(__file__).absolute().parents[3]
REFERENCE = ROOT / 'benchmark/tail-localization/measurement-baseline/verify_sample.py'
spec = importlib.util.spec_from_file_location('r033_histogram', REFERENCE)
reference = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reference)
require = reference.require


def validate_client(client, bins):
    require(client['schema'] == 'baseline-v1-client' and client['status'] == 'valid', 'client invalid')
    t0, t1 = client['T0_ns'], client['T1_ns']
    require(client['clock'] == 'CLOCK_MONOTONIC' and t1-t0 == client['duration_ns'] == 20000000000
            and t0-client['warm_start_ns'] == 5000000000, 'clock/window mismatch')
    require(len(bins) == 7 and len(client['histograms']) == 7 and client['N'] == sum(bins[0].values()) > 0, 'raw count')
    require(dict(collections.Counter(bins[1]) + collections.Counter(bins[2])) == bins[0], 'raw partitions')
    for index in (0, 1):
        denominator = sum(bins[index].values()) // 128
        require(denominator > 0, 'correction denominator')
        interval = 20000000 // denominator
        require(client['interval_us'][index] == interval and reference.reconstruct_correction(bins[index], interval) == bins[index+5], 'corrected bins')
    for index, histogram in enumerate(bins):
        row = client['histograms'][index]
        raw = bins[index-5] if index >= 5 else histogram
        low = min(raw, default=reference.UINT64_MAX)
        require(row['file'] == reference.NAMES[index] and row['count'] == sum(histogram.values())
                and row['scan_min_us'] == low and row['max_us'] == max(histogram, default=0), 'histogram metadata')
        for p in (50, 99):
            require(row['p%d_us' % p] == reference.original_percentile(histogram, low, p), 'original percentile')
    require([thread['owner'] for thread in client['threads']] == [0, 1], 'owners are not Linux TIDs')
    completed = [0]*4
    slow = []
    keys = set()
    for thread in client['threads']:
        require(thread['overflow'] is False and len(thread['slow_records']) <= thread['slow_capacity'] == 16384
                and set(thread['errors']) == {'connect','read','write','status','timeout','headers','body','protocol'}
                and all(type(value) is int and value == 0 for value in thread['errors'].values()), 'overflow/client errors')
        require(thread['stopped_ns'] >= t1 and len(thread['connections']) == 64, 'readiness/stop')
        pending = partial = unsent = deadline = pending_warm = 0
        censored = [[0]*3 for _ in range(2)]
        for position, connection in enumerate(thread['connections']):
            require(connection['life'] == position+1 and connection['ready_ns'] < client['warm_start_ns']
                    and connection['closed'] is True, '128 ready/closed lifetimes')
            reason = connection['end_reason']
            active, full, waiting = connection['active'], connection['full_sent'], connection['pending_at_T1']
            require(type(active) is bool and type(full) is bool and type(waiting) is bool, 'terminal bool')
            if waiting:
                require(client['warm_start_ns'] <= connection['start_ns'] < t1 and connection['sequence'] > 0
                        and connection['wait_lower_bound_ns'] == t1-connection['start_ns'], 'pending interval')
                pending += 1
                pending_warm += connection['start_ns'] < t0
            else:
                require(connection['wait_lower_bound_ns'] == 0, 'idle wait')
            if reason == 1:
                require(active and not full and waiting and 0 <= connection['sent_bytes'] < client['request_bytes'], 'partial censor')
                column = int(connection['sent_bytes'] > 0)
                censored[int(connection['start_ns'] >= t0)][column] += 1
                partial += column
                unsent += 1-column
            elif reason == 2:
                require(not active and not waiting, 'idle terminal')
            elif reason == 3:
                require(not active and full and waiting and connection['sent_bytes'] == client['request_bytes'], 'after completion')
            elif reason == 4:
                require(active and full and waiting and connection['start_ns']+2000000000 >= t1
                        and connection['sent_bytes'] == client['request_bytes'], 'deadline censor')
                deadline += 1
                censored[int(connection['start_ns'] >= t0)][2] += 1
            else:
                raise ValueError('terminal reason')
        require((pending, pending_warm, partial, unsent, deadline) ==
                (thread['pending_at_T1'],thread['pending_warm'],thread['censored_partial'],thread['censored_unsent'],thread['censored_sent_deadline_after_T1'])
                and censored == thread['censored_by_start_phase'], 'truncation accounting')
        for i, value in enumerate(thread['completed']):
            require(type(value) is int and value >= 0, 'completion count')
            completed[i] += value
        for record in thread['slow_records']:
            key = (thread['owner'], record['life'], record['sequence'])
            begin, end = record['start_ns'], record['complete_ns']
            require(key not in keys and 1 <= key[1] <= 64 and key[2] > 0 and client['warm_start_ns'] <= begin < end
                    and t0 <= end < t1 and 50000000 <= end-begin < 2000000000
                    and record['class'] == (1 if begin < t0 else 2) and record['status'] == 200
                    and record['body_bytes'] == 1024, 'slow record/window/body')
            keys.add(key)
            slow.append(dict(record, owner=thread['owner']))
    require(completed[1] == sum(bins[2].values()) and completed[2] == sum(bins[1].values())
            and completed[0] == sum(bins[3].values()) and completed[3] == sum(bins[4].values()), 'completion partitions')
    require(collections.Counter((r['complete_ns']-r['start_ns'])//1000 for r in slow) ==
            collections.Counter({k:v for k,v in bins[0].items() if k >= 50000}), 'complete slow raw bins')
    slow.sort(key=lambda r:(r['complete_ns'],r['start_ns'],r['owner'],r['life'],r['sequence']))
    buckets = collections.defaultdict(list)
    for record in slow:
        buckets[(record['complete_ns']-t0)//5000000].append([record['owner'],record['life'],record['sequence']])
    return {'N':client['N'],'raw_p99_us':client['histograms'][0]['p99_us'],
            'corrected_p99_us':client['histograms'][5]['p99_us'],'slow_records':slow,
            'completion_bins_5ms':[{'bin':key,'request_keys':value} for key,value in sorted(buckets.items())],
            'base_reproduced':bool(slow),'observe_permitted_by_evidence':bool(slow),
            'owner_tid_mapping_available':False,'cause_confirmed':False}


def correlate(records, snapshots, network, start, end):
    require(snapshots and network, 'missing sampling evidence')
    timelines = collections.defaultdict(list)
    last = -1
    for sample in snapshots:
        require(sample['errors'] == [] and last < sample['begin_ns'] <= sample['end_ns'], 'read failure/clock reversal')
        last = sample['begin_ns']
        for row in sample['threads']:
            key = (row['role'], row['pid'], row['process_starttime'], row['tid'], row['thread_starttime'])
            timelines[key].append((sample['begin_ns'],sample['end_ns'],row))
    gaps = []
    deltas = []
    for identity, rows in sorted(timelines.items()):
        active_start, active_end = rows[0][0], rows[-1][1]
        expected = [sample['begin_ns'] for sample in snapshots
                    if rows[0][0] <= sample['begin_ns'] <= rows[-1][0]]
        require([row[0] for row in rows] == expected, 'thread disappeared inside continuous coverage')
        require(active_start <= start and active_end >= end, 'thread measurement endpoint coverage unknown')
        for previous, current in zip(rows, rows[1:]):
            gap = current[0]-previous[1]
            delta = {key:current[2][key]-previous[2][key] for key in ('run_ticks','sched_run_ns','sched_wait_ns','voluntary','nonvoluntary','read_bytes','write_bytes')}
            require(all(value >= 0 for value in delta.values()), 'negative delta/identity reuse')
            intersect = any(record['start_ns'] < current[0] and record['complete_ns'] > previous[1] for record in records)
            if intersect and gap > 20000000:
                gaps.append({'identity':list(identity),'begin_ns':previous[1],'end_ns':current[0]})
            deltas.append({'identity':list(identity),'begin_ns':previous[1],'end_ns':current[0],
                           'delta':delta,'wchan_snapshot':current[2]['wchan'],'slow_interval_intersection':intersect})
    require(any(key[0]=='server' for key in timelines) and any(key[0]=='client' for key in timelines)
            and any(key[0]=='observer' for key in timelines), 'target/self threads missing')
    last = -1
    for row in network:
        require(row['errors'] == [] and last < row['begin_ns'] <= row['end_ns'], 'network read/clock')
        last = row['begin_ns']
    require(network[0]['begin_ns'] <= start and network[-1]['end_ns'] >= end, 'network endpoint missing')
    network_gaps = [{'begin_ns':a['end_ns'],'end_ns':b['begin_ns']} for a,b in zip(network,network[1:])
                    if b['begin_ns']-a['end_ns'] > 200000000]
    return {'direction_status':'unknown' if gaps or network_gaps else 'descriptive_evidence',
            'thread_gaps':gaps,'network_gaps':network_gaps,'thread_deltas':deltas,
            'network_background_may_include_other_processes':True,'wchan_is_instantaneous':True,
            'server_request_RD_available':False,'cause_confirmed':False}


def analyze_sample(directory):
    directory=Path(directory)
    sample = json.loads(reference.read_regular(directory/'sample.json', 4*1024*1024))
    require(sample['schema']=='r033-d-sample-v1' and sample['status']=='valid', 'sample invalid')
    require(sample['server_commit']=='69424e6ab057bba2950c018e34c5695a4dc74f22'
            and sample['server_sha256']=='46cb6a39410b819f1b1f156c3db9d2eb9f4da8ef73c8f61f979c8ae05125b240'
            and sample['client_sha256']=='1f0065792d9370fd14689caa7a9df5247f44bbb4c2066d780efbd94001adf348'
            and sample['cleanup']['complete'] is True and sample['first_error'] is None and sample['errors']==[], 'fixed D/client/cleanup identity')
    require(sample['mode'] in ('base','observe'),'sample mode')
    for audits in (sample['result']['pre_audit'],sample['result']['post_audit']):
        require(len(audits)==5 and all(row['status']==200 and row['bytes']==1024 and
                row['sha256']=='785b0751fc2c53dc14a4ce3d800e69ef9ce1009eb327ccf458afe09c242c26c9' for row in audits),'HTTP audit')
    catalog_paths=set()
    for entry in sample['catalog']:
        path = Path(entry['path'])
        require(not path.is_absolute() and '..' not in path.parts and entry['path'] not in catalog_paths, 'catalog escape/duplicate')
        catalog_paths.add(entry['path'])
        raw = reference.read_regular(directory/path, 256*1024*1024)
        require(len(raw)==entry['bytes'] and hashlib.sha256(raw).hexdigest()==entry['sha256'], 'catalog drift')
    require(set(reference.NAMES)|{'client.json','actual-command.json'} <= catalog_paths,'complete raw catalog')
    client = json.loads(reference.read_regular(directory/'client.json',4*1024*1024))
    result = validate_client(client,[reference.decode_bins(reference.read_regular(directory/name,4*1024*1024)) for name in reference.NAMES])
    if sample['mode']=='observe':
        require({'threads.jsonl','network.jsonl'} <= catalog_paths,'observation catalog missing')
        load = lambda name:[json.loads(line) for line in reference.read_regular(directory/name,192*1024*1024).splitlines()]
        result['correlation'] = correlate(result['slow_records'],load('threads.jsonl'),load('network.jsonl'),client['T0_ns'],client['T1_ns'])
        require(result['correlation']['direction_status']=='descriptive_evidence','observation coverage unknown')
    result.update(mode=sample['mode'],sample_sha256=hashlib.sha256(reference.read_regular(directory/'sample.json',4*1024*1024)).hexdigest(),cause_confirmed=False)
    return result


def main():
    import time
    parser=argparse.ArgumentParser()
    parser.add_argument('--role',choices=('builder','reviewer'),required=True)
    parser.add_argument('--output',required=True)
    parser.add_argument('--sample',action='append',required=True)
    parser.add_argument('--shared-work-deadline-ns',type=int,required=True)
    parser.add_argument('--shared-cleanup-deadline-ns',type=int,required=True)
    args=parser.parse_args()
    stage=ROOT/'.cache/v0.5.1-revalidation'
    output=Path(args.output).absolute()
    require(output==stage/args.role/'control/r034-diagnose-offline-driver-001','exact offline output')
    allowed=[stage/'builder/control/r034-diagnose-base-driver-001',stage/'builder/control/r034-diagnose-observe-driver-001']
    directories=[Path(value).absolute() for value in args.sample]
    require(directories in (allowed[:1],allowed),'exact ordered raw directories')
    require(time.monotonic_ns()<args.shared_work_deadline_ns<args.shared_cleanup_deadline_ns,'offline common deadline')
    output.mkdir()
    result={'schema':'r034-offline-v1','status':'invalid','role':args.role,'samples':[],'first_error':None,'cause_confirmed':False}
    failure=None
    try:
        for directory in directories:
            require(time.monotonic_ns()<args.shared_work_deadline_ns,'offline work deadline')
            result['samples'].append(analyze_sample(directory))
        require((len(directories)==2)==bool(result['samples'][0]['slow_records']),'conditional observed sample binding')
        result['status']='valid'
    except BaseException as error:
        failure=error
        result['first_error']={'type':type(error).__name__}
    finally:
        try:
            with (output/'offline-result.json').open('x') as stream:
                json.dump(result,stream,sort_keys=True)
        except BaseException as error:
            if failure is None:failure=error
            try:sys.stderr.write('offline publication failed: '+type(error).__name__+'\n')
            except BaseException:pass
    if failure is not None:raise failure


if __name__=='__main__':
    main()
