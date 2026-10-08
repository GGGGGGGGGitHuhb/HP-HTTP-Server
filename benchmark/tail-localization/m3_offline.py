"""Bounded original M3 evidence check; A headers, B exact selected request chains."""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import signal
import time
from decode_v3 import HEADER,decode,thread_records,demand
from m3_metrics import summary
from wire_types_v3 import Control

ROOT=Path(__file__).resolve().parents[2]


def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def verify(directory):
    directory=Path(directory);sample=json.loads((directory/'sample.json').read_text())
    demand(sample['status']=='valid' and sample['schema']==3 and sample['identity_scope']=='kernel-mapped' and sample['kernel_mapping_verified'] is True,'M3 capture invalid')
    demand(sample['parameters']==dict(requests_per_connection=0,connections=128,warmup=5,duration=20,detailed=sample['parameters']['detailed']),'M3 parameters differ')
    demand(sample['recording_end_ns']>=sample['measurement_end_ns'] and sample['measurement_end_ns']-sample['measurement_start_ns']==20_000_000_000,'M3 measurement stopped early')
    demand(len(sample['frozen_connections'])==4,'M3 selection differs')
    cleanup=sample['cleanup'];demand(not cleanup['forced'] and not cleanup['errors'] and not cleanup['remaining'] and not cleanup.get('unknown'),'M3 cleanup invalid')
    raw=(directory/'startup-control.bin').read_bytes();demand(len(raw)==ctypes.sizeof(Control),'control size differs');control=Control.from_buffer_copy(raw)
    demand(control.magic==b'S4CTRL03' and control.version==3 and control.phase==5 and not control.abortRun and control.firstFailure.published==0,'M3 terminal control invalid')
    demand(control.connections==128 and control.selectedCount==4 and control.serverRegistered==control.clientRegistered==control.serverAttempts==control.clientAttempts==128,'M3 registrations differ')
    demand(control.warmupNs==5_000_000_000 and control.measurementNs==20_000_000_000 and bool(control.detailed)==sample['parameters']['detailed'],'M3 control configuration differs')
    for table in (control.serverRegistrations,control.clientRegistrations):demand(all(row.ready==1 for row in table),'M3 registration evidence incomplete')
    for table in (list(control.serverThreads)[:4],list(control.clientThreads)[:2]):demand(all(row.stoppedNs>=sample['measurement_end_ns'] for row in table),'M3 writers did not stop after measurement')
    performance=summary((directory/'client.stdout').read_text(),0)
    demand(all(sample['performance'].get(key)==value for key,value in performance.items()),'summary metadata drift')
    marker=json.loads((directory/'marker-launcher.json').read_text());demand(marker['status']=='valid' and marker['marker_lost_events']==0,'kernel startup evidence invalid')
    if sample['parameters']['detailed']:result=decode(directory)
    else:
        mapping={(item['endpoint'],item['worker']):item for item in sample['kernel_mappings']};catalog=[]
        for endpoint,count in [('server',4),('client',2)]:
            for worker in range(count):
                path=directory/f'{endpoint}-worker-{worker}.events.bin'
                demand(not list(thread_records(path,endpoint,mapping[(endpoint,worker)],sample,catalog)),'A detailed-disabled writer emitted events')
        for endpoint in ('server','client'):
            clock=json.loads((directory/f'{endpoint}-clock.json').read_text());main=next(x for x in sample['kernel_mappings'] if x['endpoint']==endpoint and x['role']=='main')
            demand(clock['pid']==main['pid'] and clock['boot_id']==main['identity']['boot_id'] and clock['time_namespace']==main['identity']['time_namespace'] and clock['clock']=='CLOCK_MONOTONIC' and clock['unit']=='ns','A endpoint clock identity differs')
        result=dict(schema=3,status='valid',run_id=directory.name,role=sample['manifest']['role'],file_catalog=catalog,sample_sha256=digest(directory/'sample.json'),manifest_sha256=hashlib.sha256(json.dumps(sample['manifest'],sort_keys=True,separators=(',',':')).encode()).hexdigest(),parameters=sample['parameters'],identity_scope='kernel-mapped',kernel_mapping_verified=True,selected_connections=4,overflow=False,cleanup=cleanup,complete_requests=None,measurement_slow_requests=[],limits=['A has no request events; global raw histogram remains available; no fabricated request chains'])
    result.update(performance=performance,resources=sample['performance']['resources'],slowest_measurement_requests=result['measurement_slow_requests'][:5],raw_distribution_scope='global all128 histogram; selected4 chains are separate')
    return result


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--role',choices=('builder','reviewer'),required=True);parser.add_argument('--input',type=Path,required=True);parser.add_argument('--run-id',required=True);args=parser.parse_args()
    role=ROOT/'.cache/v0.5.1-s4'/args.role;directory=args.input.absolute()
    demand(directory.parent==role and not any(p.is_symlink() for p in (directory,*directory.parents)),'offline input outside own ordinary run')
    captures=tuple(f'run-m3-abba-{n:02d}' for n in range(1,5)) if args.role=='builder' else tuple(f'run-m3-confirm-{n:02d}' for n in range(1,3))
    demand(directory.name in captures and args.run_id=='run-m3-offline-'+directory.name.removeprefix('run-m3-'),'offline run binding differs')
    ledger=json.loads((role/'ledger.json').read_text());rows=[r for r in ledger['runs'] if r['status']=='running'];demand(len(rows)==1 and rows[0]['run_id']==args.run_id and rows[0]['kind']=='offline' and rows[0]['reserved_seconds']==30,'offline reservation differs')
    deadline=rows[0]['start_monotonic']+30-7;demand(time.monotonic()<deadline,'offline deadline expired')
    def expired(number,frame):raise TimeoutError('M3 offline absolute work deadline')
    saved=signal.signal(signal.SIGALRM,expired);signal.setitimer(signal.ITIMER_REAL,max(0,deadline-time.monotonic()))
    try:(directory/'association.json').write_text(json.dumps(verify(directory),indent=2)+'\n')
    finally:signal.setitimer(signal.ITIMER_REAL,0);signal.signal(signal.SIGALRM,saved)

if __name__=='__main__':main()
