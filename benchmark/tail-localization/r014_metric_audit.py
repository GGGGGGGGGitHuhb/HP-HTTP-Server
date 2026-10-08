"""R014 offline source-bound summary recomputation; no workload or host reads."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import time

ROOT=Path(__file__).resolve().parents[2]
RUN='run-m3-oa-audit-001'

def digest(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--role',choices=('builder','reviewer'),required=True);parser.add_argument('--inputs',type=Path,required=True);parser.add_argument('--inputs-sha256',required=True)
    args=parser.parse_args();role=ROOT/'.cache/v0.5.1-s4'/args.role
    rows=json.loads((role/'ledger.json').read_text())['runs'];active=[r for r in rows if r['status']=='running']
    if len(active)!=1 or active[0]['run_id']!=RUN or active[0]['kind']!='offline' or active[0]['reserved_seconds']!=20 or active[0]['output']!=str(role/RUN):raise ValueError('metric audit exact offline20 reservation required')
    deadline=active[0]['start_monotonic']+13
    if digest(args.inputs)!=args.inputs_sha256:raise ValueError('metric audit input drift')
    inputs=json.loads(args.inputs.read_text())
    if inputs['role']!=args.role:raise ValueError('metric audit role differs')
    for item in inputs['files']:
        if time.monotonic()>=deadline:raise TimeoutError('metric audit original deadline')
        path=Path(item['path'])
        if not path.is_absolute() or '..' in path.parts or not path.is_relative_to(ROOT) or any(p.is_symlink() for p in (path,*path.parents)):raise ValueError('metric audit source path unsafe')
        if digest(path)!=item['sha256']:raise ValueError('metric audit frozen source/raw drift')
    result=[]
    for sample in inputs['samples']:
        if time.monotonic()>=deadline:raise TimeoutError('metric audit original deadline')
        path=Path(sample['path']);value=json.loads(path.read_text())
        measurement=value['sample']['measurement'] if sample['type']=='O' else value['performance']
        numerator=measurement['requests'];duration=measurement['duration_us'];errors=measurement['errors'];latency=measurement['latency_us']
        if type(numerator)!=int or numerator<=0 or type(duration) not in (int,float) or not math.isfinite(duration) or duration<=0 or int(duration)!=duration:raise ValueError('metric summary invalid numerator/duration')
        if any(errors[name]!=0 for name in ('connect','read','write','status','timeout')):raise ValueError('metric summary transport error')
        qps=numerator*1_000_000/duration
        if not math.isclose(qps,measurement['qps'],rel_tol=1e-12):raise ValueError('existing QPS differs from actual N/T')
        per_connection=numerator//128
        if not per_connection:raise ValueError('correction interval divisor zero')
        interval=int(duration)//per_connection
        corrected={name:latency[name]/1000 for name in ('p50','p99','max')}
        if any(not math.isfinite(x) or x<0 for x in corrected.values()) or not corrected['p50']<=corrected['p99']<=corrected['max']:raise ValueError('corrected summary invalid')
        result.append(dict(run_id=sample['run_id'],type=sample['type'],source_path=str(path),source_sha256=digest(path),requests=numerator,duration_us=duration,qps_recomputed=qps,correction_interval_us_recomputed=interval,correction_interval_numerator='runtime_us including thread join',correction_interval_denominator=floor_text(numerator),corrected_latency_ms=corrected,original_latency_buckets_available=False,percentile_independently_recomputed=False,raw_latency_ms=measurement.get('raw_latency_ms') if sample['type']=='A' else None))
    output=dict(schema=1,role=args.role,run_id=RUN,status='audit_completed',inputs_sha256=args.inputs_sha256,helper_sha256=digest(Path(__file__)),command_sha256=digest(role/RUN/'command.json'),source_bindings=inputs['files'],metric_contract=inputs['metric_contract'],samples=result,ac01='BLOCKED',reason='different completion populations at warmup/end boundaries; corrected synthetic weight cannot be bounded from preserved O buckets/timestamps because they are absent',limits=['same formula and same stats code do not prove equal window population','no original raw or histogram buckets reconstructed from diagnostic raw','no new capture, compile, mock, host/proc/socket probe or binary modification'])
    if time.monotonic()>=deadline:raise TimeoutError('metric audit original deadline')
    with (role/RUN/'metric-audit.json').open('x') as stream:json.dump(output,stream,indent=2);stream.write('\n')

def floor_text(numerator):return dict(connections=128,complete_per_connection_integer=numerator//128)

if __name__=='__main__':main()
