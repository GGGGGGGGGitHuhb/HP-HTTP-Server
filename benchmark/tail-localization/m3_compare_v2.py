"""Original staircase-versus-A and A/B warnings; no new samples or invented causes."""
import argparse
import hashlib
import json
from pathlib import Path
import signal
import statistics
import time
from m3_contract_v2 import ROOT,STAIRS,ABBA,CONFIRM,require,check_history
from m3_environment_v2 import validate as validate_environment


def percent(a,b):
    require(a>0 and b>=0,'invalid comparison baseline')
    return abs(b-a)/a*100


def compare(stairs,observed):
    a=[row['performance'] for row in observed if not row['parameters']['detailed']]
    b=[row['performance'] for row in observed if row['parameters']['detailed']]
    require(a and b,'A/B pair missing')
    median=lambda rows,key:statistics.median(key(row) for row in rows)
    changes=dict(qps_percent=percent(median(a,lambda x:x['qps']),median(b,lambda x:x['qps'])),global_raw_p99_percent=percent(median(a,lambda x:x['raw_latency_ms']['p99']),median(b,lambda x:x['raw_latency_ms']['p99'])))
    warnings=[]
    if changes['qps_percent']>15 or changes['global_raw_p99_percent']>25:warnings.append('A/B perturbation or natural variation not separated')
    original=[row['sample']['measurement'] for row in stairs if row['connections']==128]
    cross=None
    if stairs:
        require(len(original)==2,'both original 128 staircase rows required')
        cross=dict(qps_percent=percent(median(original,lambda x:x['qps']),median(a,lambda x:x['qps'])),corrected_p99_percent=percent(median(original,lambda x:x['latency_ms']['p99']),median(a,lambda x:x['corrected_latency_ms']['p99'])),original_raw_available=False)
        if cross['qps_percent']>15 or cross['corrected_p99_percent']>25:warnings.append('diagnostic construction/client change or natural variation not separated')
    slow=[dict(run_id=row['run_id'],**request) for row in observed for request in row['measurement_slow_requests']]
    slow.sort(key=lambda x:x['raw_latency_ns'],reverse=True)
    return dict(status='valid',comparison=changes,original128_vs_a=cross,warnings=warnings,perturbation_separated=not warnings,measurement_slow_requests=slow,slowest_five=slow[:5],core_evidence_candidate=bool(slow) and not warnings,requires_independent_interval_review=True,limits=['no slow chain or unresolved perturbation blocks core S4 evidence','global all128 raw histogram differs from selected4 request chains','original wrk has only corrected latency; do not compare it as raw','two repeated rows cannot establish statistical significance or CPU capacity','signed spans and residual_unknown are not proof of lock/IO/network/CPU cause'])


def verify_binding(directory,row,role,run):
    sample_path=directory/'sample.json';sample=json.loads(sample_path.read_text())
    require(row['status']=='valid' and row['run_id']==run and row['role']==role and row['sample_sha256']==hashlib.sha256(sample_path.read_bytes()).hexdigest(),'association binding drift')
    canonical=hashlib.sha256(json.dumps(sample['manifest'],sort_keys=True,separators=(',',':')).encode()).hexdigest()
    require(row['manifest_sha256']==canonical,'association manifest binding drift')
    expected={str(directory/f'{endpoint}-worker-{worker}.events.bin') for endpoint,count in [('server',4),('client',2)] for worker in range(count)}
    require(len(row['file_catalog'])==6 and {item['path'] for item in row['file_catalog']}==expected,'raw catalog scope differs')
    for item in row['file_catalog']:
        path=Path(item['path']);require(path.is_absolute() and '..' not in path.parts and not any(p.is_symlink() for p in (path,*path.parents)),'raw catalog symlink or noncanonical path')
        digest=hashlib.sha256()
        with path.open('rb') as stream:
            while True:
                block=stream.read(1024*1024)
                if not block:break
                digest.update(block)
        require(digest.hexdigest()==item['sha256'],'raw catalog bytes changed since offline association')
    check_history(sample['manifest']['r012_history'])
    for name,phase,owner,uid,gid in [('root-environment.json','root-before-resources',json.loads((directory/'marker-launcher.json').read_text())['root_identity'],0,0),('power-environment.json','power-before-load-and-go',sample['route']['identity'],1000,1000)]:
        path=directory/name;require(not path.is_symlink(),'environment evidence symlink')
        require(row['environment_sha256'][name]==hashlib.sha256(path.read_bytes()).hexdigest(),'environment evidence drift')
        validate_environment(json.loads(path.read_text()),role,run,phase,sample['manifest'],owner,uid,gid)
    return row


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--role',choices=('builder','reviewer'),required=True);args=parser.parse_args();role=ROOT/'.cache/v0.5.1-s4'/args.role
    ledger=json.loads((role/'ledger.json').read_text());running=[row for row in ledger['runs'] if row['status']=='running'];require(len(running)==1 and running[0]['run_id']=='run-m3-compare-001' and running[0]['kind']=='offline' and running[0]['reserved_seconds']==20,'comparison reservation differs')
    deadline=running[0]['start_monotonic']+20-7;require(time.monotonic()<deadline,'comparison deadline expired')
    def expired(number,frame):raise TimeoutError('M3 compare absolute deadline')
    saved=signal.signal(signal.SIGALRM,expired);signal.setitimer(signal.ITIMER_REAL,max(0,deadline-time.monotonic()))
    try:
        captures=ABBA if args.role=='builder' else CONFIRM
        observed=[]
        for run in captures:
            capture=[row for row in ledger['runs'] if row['run_id']==run];offline=[row for row in ledger['runs'] if row['run_id']=='run-m3-offline-'+run.removeprefix('run-m3-')]
            require(len(capture)==len(offline)==1 and capture[0]['status']==offline[0]['status']=='valid','capture/offline incomplete')
            directory=role/run;require(not any(p.is_symlink() for p in (directory,*directory.parents)),'comparison input symlink')
            row=json.loads((directory/'association.json').read_text());observed.append(verify_binding(directory,row,args.role,run))
        stairs=[json.loads((role/run/'staircase.json').read_text()) for run in STAIRS] if args.role=='builder' else []
        if stairs:require(all(row['status']=='valid' for row in stairs),'staircase invalid')
        (role/'run-m3-compare-001'/'comparison.json').write_text(json.dumps(compare(stairs,observed),indent=2)+'\n')
    finally:signal.setitimer(signal.ITIMER_REAL,0);signal.signal(signal.SIGALRM,saved)

if __name__=='__main__':main()
