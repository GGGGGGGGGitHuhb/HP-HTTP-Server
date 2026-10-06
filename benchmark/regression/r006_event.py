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
    parser.add_argument('--preload',type=pathlib.Path,required=True); parser.add_argument('--variant', type=pathlib.Path, required=True); parser.add_argument('--output', type=pathlib.Path, required=True); args = parser.parse_args()
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
                           '-ex', 'set environment LD_PRELOAD ' + str(args.preload),
                           '-ex', 'set environment R006_WAIT_OUTPUT ' + str(prefix) + '.wait.bin',
                           '-ex', 'run', '--args', *command]
            previous={key:os.environ.get(key) for key in ('LD_PRELOAD','R006_WAIT_OUTPUT')}
            try:
                if pathlib.Path(prefix).name == 'server':
                    os.environ['LD_PRELOAD']=str(args.preload); os.environ['R006_WAIT_OUTPUT']=str(prefix)+'.wait.bin'
                super().__init__(command,prefix); owned.append(self)
            finally:
                for key,value in previous.items():
                    if value is None: os.environ.pop(key,None)
                    else: os.environ[key]=value
        def alive(self):
            alive=super().alive()
            if alive and self.command[0]=='/usr/bin/gdb':
                try:
                    maps=pathlib.Path('/proc/'+str(self.process.pid)+'/maps').read_text()
                    if '/usr/bin/gdb' in maps:
                        legacy.demand(str(args.preload) not in maps,'GDB body unexpectedly injected')
                        path=pathlib.Path(str(self.stdout_path).removesuffix('.stdout')+'.gdb.maps')
                        if not path.exists(): path.write_text(maps)
                except FileNotFoundError: pass
            return alive
    executor.OwnedProcess = Observed
    record = dict(status='invalid',samples=[],preload=str(args.preload),preload_sha256=legacy.sha(args.preload))
    try:
        before = batch.snapshot('builder',args.wrk); record['identity_before'] = before
        fixture = args.output / 'root'; fixture.mkdir(); payload = legacy.fixture(fixture,1024)
        variant = dict(before['manifests']['E'])
        variant.update(label='E-no-response-info-diagnostic', source=str(args.variant/'source'), binary=str(args.variant/'build/hp_http_server'))
        variant['binary_sha256'] = legacy.sha(variant['binary'])
        source_hashes = {name:legacy.sha(args.variant/'source'/name) for name in variant['source_hashes']}
        changes = [name for name,digest in source_hashes.items() if digest != variant['source_hashes'][name]]
        legacy.demand(changes == ['app/HttpConnectionHandler.cpp'],'diagnostic source scope drift')
        original_source=pathlib.Path(before['manifests']['E']['source'])/'app/HttpConnectionHandler.cpp'
        call='  base::info(\n      "S3 evidence: HTTP message callback produced one response via "\n      "incremental parser.");'
        legacy.demand(original_source.read_text().count(call)==1,'diagnostic call not unique')
        expected_source=original_source.read_text().replace(call,'  // R006 diagnostic only: omit per-response evidence info to test the logging path.')
        legacy.demand((args.variant/'source/app/HttpConnectionHandler.cpp').read_text()==expected_source,'diagnostic source differs beyond approved call')
        variant['source_hashes'] = source_hashes
        variant['diagnostic_only'] = True
        variant['compile_commands'] = str(args.variant/'build/compile_commands.json')
        variant['cmake_cache'] = str(args.variant/'build/CMakeCache.txt')
        for key in ('compile_commands','cmake_cache'): variant[key+'_sha256'] = legacy.sha(variant[key])
        def cache_flags(path):
            return sorted(line for line in pathlib.Path(path).read_text().splitlines() if line.startswith(('CMAKE_CXX_FLAGS:', 'CMAKE_CXX_FLAGS_RELEASE:', 'CMAKE_BUILD_TYPE:')))
        legacy.demand(cache_flags(variant['cmake_cache'])==cache_flags(before['manifests']['E']['cmake_cache']),'CMake flags differ')
        for entry in json.loads(pathlib.Path(variant['compile_commands']).read_text()):
            compiled=pathlib.Path(entry['file']); variant_source=pathlib.Path(variant['source'])
            legacy.demand(compiled.is_relative_to(variant_source) and str(compiled.relative_to(variant_source)) in source_hashes,'compiled file outside diagnostic source')
            legacy.demand(all(flag in entry['command'] for flag in ('-O3','-DNDEBUG','-std=c++20')) and not any(flag in entry['command'] for flag in ('-fsanitize','-flto','-march','-mtune')), 'diagnostic Release flags changed')
        record['variant'] = variant; record['variant_source_hashes'] = source_hashes
        deadline = time.monotonic() + 150
        for index in range(1,3):
            item = dict(round=index, scenario='P3', label='E', **model.SCENARIOS['P3'])
            manifest = variant
            item['label'] = manifest['label']
            directory = args.output / f'{index:02d}-P3-{manifest["label"]}'
            row = executor.run_sample(manifest,args.wrk,fixture,payload,directory,root,deadline,item,5,20)
            result = verify(directory / 'measurement',row)
            result['label'] = manifest['label']
            legacy.demand(str(args.preload) not in (directory/'measurement.gdb.maps').read_text(),'GDB preload leaked')
            for name,pid in (('server',row['server_identity']['pid']),('measurement',result['capture_metadata']['pid'])):
                trace=directory/(name+'.wait.bin'); data=trace.read_bytes(); header=struct.unpack_from('<8Q',data); slot_size=184+2048*64
                legacy.demand(header[:5]==(0x5230303657414954,1,slot_size,16,2048) and header[5]<=16 and header[6]==pid and header[7]==1,'event trace header/identity invalid')
                legacy.demand(len(data)==64+16*slot_size,'event trace truncated')
                for slot in range(16):
                    values=struct.unpack_from('<23Q',data,64+slot*slot_size)
                    legacy.demand(values[1]<=2048 and values[2]==0,'event trace overflow')
                    for event in range(values[1]):
                        fields=struct.unpack_from('<8Q',data,64+slot*slot_size+184+event*64)
                        legacy.demand(fields[0]==values[0] and 1<=fields[1]<=9 and fields[3]>=5000000,'event trace record invalid')
                legacy.demand(str(args.preload) in pathlib.Path(str(trace)+'.maps').read_text(),'event preload absent')
            record['samples'].append(result); legacy.save(args.output / 'diagnosis.json',record)
        record['identity_after'] = batch.snapshot('builder',args.wrk)
        legacy.demand(before == record['identity_after'],'identity drift')
        for key in ('compile_commands','cmake_cache'): legacy.demand(variant[key+'_sha256']==legacy.sha(variant[key]), 'diagnostic build identity drift')
        legacy.demand(variant['binary_sha256']==legacy.sha(variant['binary']),'variant binary drift')
        legacy.demand(source_hashes=={name:legacy.sha(args.variant/'source'/name) for name in source_hashes},'variant source drift')
        legacy.demand(record['preload_sha256']==legacy.sha(args.preload),'preload drift')
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
