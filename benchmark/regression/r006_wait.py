#!/usr/bin/env python3
"""R006 joined-thread histogram diagnosis; execute under R005 supervised check."""
import argparse
import array
import json
import os
import pathlib
import struct
import time
import zlib
import batch_candidate as batch
import executor
import model
from identity import legacy


def decode(path):
    data = zlib.decompress(path.read_bytes())
    header = struct.unpack('<4Q', data[:32])
    bins = array.array('Q'); bins.frombytes(data[32:])
    legacy.demand(len(bins) == header[1] and sum(bins) == header[0], 'offline histogram size/count')
    return header, bins


def percentile(header, bins, p):
    rank = (header[0] * p) // 100 + 1
    total = 0
    for i in range(header[2], header[3] + 1):
        total += bins[i]
        if total >= rank:
            return i / 1000
    return 0


def verify(prefix, row):
    meta = json.loads(pathlib.Path(str(prefix) + '.histogram.json').read_text())
    raw, bins = decode(pathlib.Path(str(prefix) + '.raw.zlib'))
    corrected, actual = decode(pathlib.Path(str(prefix) + '.corrected.zlib'))
    expected = meta['expected']
    for name, header in (('raw',raw),('corrected',corrected)):
        legacy.demand([meta[name][key] for key in ('count','limit','minimum','maximum')] == list(header), 'metadata/header mismatch')
    legacy.demand(raw[1:] == corrected[1:], 'fixed header fields changed')
    legacy.demand(meta['threads'] == 1 and int(meta['starttime']) > 0 and meta['pid'] > 0, 'inferior identity missing')
    simulated = array.array('Q', bins)
    count = raw[0]
    for n in range(expected * 2, raw[3] + 1):
        amount = simulated[n]
        m = n - expected
        while amount and m > expected:
            simulated[m] += amount; count += amount; m -= expected
    legacy.demand(simulated == actual and count == corrected[0], 'exact correction replay mismatch')
    measurement = row['measurement']
    legacy.demand(raw[0] == measurement['requests'], 'raw observations differ from completed requests')
    legacy.demand(expected == measurement['duration_us'] // (measurement['requests'] // 128), 'actual integer expected differs')
    legacy.demand(percentile(corrected, actual, 99) == measurement['latency_ms']['p99'], 'corrected p99 differs from summary')
    legacy.demand(corrected[3]/1000 == measurement['latency_ms']['max'], 'corrected max differs from summary')
    legacy.demand(all(value == 0 for value in measurement['errors'].values()), 'request errors')
    result = dict(capture_metadata=meta, observation_seconds=meta['capture_end_monotonic']-meta['entry_monotonic'], expected_us=expected, raw_count=raw[0], corrected_count=corrected[0],
                  raw_p99_ms=percentile(raw, bins, 99), corrected_p99_ms=percentile(corrected, actual, 99),
                  raw_max_ms=raw[3]/1000, corrected_max_ms=corrected[3]/1000,
                  raw_p50_ms=percentile(raw,bins,50), raw_p90_ms=percentile(raw,bins,90),
                  exact_bucket_replay=True, measurement=measurement)
    legacy.save(str(prefix) + '.comparison.json', result)
    return result


def main():
    parser = argparse.ArgumentParser(); parser.add_argument('--wrk', type=pathlib.Path, required=True)
    parser.add_argument('--preload', type=pathlib.Path, required=True); parser.add_argument('--output', type=pathlib.Path, required=True); args = parser.parse_args()
    args.output.mkdir()
    root = batch.identity.role_root('builder')
    auth = batch.authorization('builder')
    def guard(*unused):
        size = legacy.log_bytes(root)
        legacy.demand(auth['baseline']['builder']['log_bytes'] <= size <= auth['baseline']['builder']['log_bytes'] + auth['additional_log_bytes_per_role'], 'supplement log guard')
    legacy.log_guard = guard; executor.log_guard = guard
    original = executor.OwnedProcess
    owned = []
    class Observed(original):
        def __init__(self, command, prefix):
            if pathlib.Path(prefix).name == 'measurement':
                os.environ['R006_CAPTURE_PREFIX'] = str(prefix)
                command = ['/usr/bin/gdb', '--nx', '--nh', '--batch', '-q',
                           '-ex', 'set pagination off', '-ex', 'set confirm off',
                           '-ex', 'set debuginfod enabled off', '-ex', 'set auto-load off',
                           '-ex', 'set disable-randomization off',
                           '-ex', 'source ' + str(pathlib.Path(__file__).with_name('r006_gdb_capture.py').resolve()),
                           '-ex', 'run', '--args', *command]
            previous = {key:os.environ.get(key) for key in ('LD_PRELOAD','R006_WAIT_OUTPUT')}
            try:
                if pathlib.Path(prefix).name == 'server':
                    os.environ['LD_PRELOAD'] = str(args.preload)
                    os.environ['R006_WAIT_OUTPUT'] = str(prefix) + '.wait.bin'
                super().__init__(command,prefix); owned.append(self)
            finally:
                for key,value in previous.items():
                    if value is None: os.environ.pop(key,None)
                    else: os.environ[key]=value
    executor.OwnedProcess = Observed
    record = dict(status='invalid', samples=[], preload=str(args.preload), preload_sha256=legacy.sha(args.preload), observation_variant='E with syscall/mutex duration preload; diagnostic only')
    try:
        before = batch.snapshot('builder',args.wrk); record['identity_before'] = before
        fixture = args.output / 'root'; fixture.mkdir(); payload = legacy.fixture(fixture,1024)
        deadline = time.monotonic() + 150
        for index in range(1,3):
            item = dict(round=index, scenario='P3', label='E', **model.SCENARIOS['P3'])
            directory = args.output / f'{index:02d}-P3-E'
            row = executor.run_sample(before['manifests']['E'],args.wrk,fixture,payload,directory,root,deadline,item,5,20)
            result = verify(directory / 'measurement',row)
            data=(directory/'server.wait.bin').read_bytes()
            header=struct.unpack_from('<8Q',data)
            slot_size=136+2048*64
            legacy.demand(header[:5]==(0x5230303657414954,1,slot_size,16,2048) and header[5]<=16 and header[6]==row['server_identity']['pid'] and header[7]==1, 'wait header/identity invalid')
            legacy.demand(len(data)==64+16*slot_size,'wait dump truncated')
            for slot in range(16):
                values=struct.unpack_from('<17Q',data,64+slot*slot_size)
                legacy.demand(values[1]<=2048 and values[2]==0,'wait events overflow')
                for event in range(values[1]):
                    fields=struct.unpack_from('<8Q',data,64+slot*slot_size+136+event*64)
                    legacy.demand(fields[0]==values[0] and 1<=fields[1]<=6 and fields[3]>=5000000,'wait event invalid')
            legacy.demand(str(args.preload) in (directory/'server.wait.bin.maps').read_text(),'preload mapping missing')
            record['samples'].append(result); legacy.save(args.output / 'diagnosis.json',record)
        record['identity_after'] = batch.snapshot('builder',args.wrk)
        legacy.demand(before == record['identity_after'],'identity drift')
        legacy.demand(record['preload_sha256'] == legacy.sha(args.preload),'preload drift')
        record['status'] = 'valid'
    finally:
        record['cleanup'] = []
        for child in owned:
            try:
                cleanup = child.close(); record['cleanup'].append(cleanup)
                if cleanup['forced'] or not cleanup['reaped']:
                    record['status'] = 'invalid'
            except BaseException as error:
                record['cleanup'].append({'error':str(error)}); record['status']='invalid'
        legacy.save(args.output / 'diagnosis.json', record)
    print(json.dumps(dict(status=record['status'], samples=[{k:v for k,v in sample.items() if k != 'measurement'} for sample in record['samples']]),indent=2))

    legacy.demand(record['status'] == 'valid','diagnosis invalid after cleanup')

if __name__ == '__main__': main()
