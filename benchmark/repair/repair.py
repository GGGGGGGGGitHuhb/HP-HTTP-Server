#!/usr/bin/env python3
"""S2 fixed C / explicit uncommitted D snapshot build and paired verification."""
import argparse
import hashlib
import io
import json
import pathlib
import shutil
import statistics
import subprocess
import sys
import tarfile
import time

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'benchmark'))
import build as builds
import run as bench

BASE = '942f72cd9cea58e097025c3b9dd660f4132a1ffb'
builds.COMMITS['C'] = BASE
builds.TREES['C'] = 'b407f052c7a974ae4fff4976c8275fb5905cf036'
builds.COMMITS['D'] = None
builds.TREES['D'] = None
original_archive = builds.archive_bytes


def product_paths():
    tracked = subprocess.check_output(['git','ls-files','-z','app','src','include','tests','CMakeLists.txt'],cwd=REPO).decode().split('\0')
    return sorted(set(filter(None, tracked)) | {'tests/TcpNoDelay_test.cpp','tests/TcpNoDelayHttp_test.py'})


def workspace_hashes():
    return {name: builds.sha(REPO/name) for name in product_paths()}


def candidate_archive():
    contents = {}
    with tarfile.open(fileobj=io.BytesIO(original_archive('C'))) as source:
        for entry in source:
            if entry.isfile():
                contents[entry.name] = source.extractfile(entry).read()
    for name in product_paths():
        contents[name] = (REPO/name).read_bytes()
    output = io.BytesIO()
    with tarfile.open(fileobj=output,mode='w') as archive:
        for name, content in sorted(contents.items()):
            entry=tarfile.TarInfo(name)
            entry.size=len(content)
            entry.mode=0o755 if name.endswith('.sh') else 0o644
            archive.addfile(entry,io.BytesIO(content))
    return output.getvalue()


def archive_bytes(label):
    return candidate_archive() if label=='D' else original_archive(label)


builds.archive_bytes = archive_bytes
bench.archive_bytes = archive_bytes


def role_root(role):
    return REPO/'.cache/v0.5.1-s2'/role


def tool_identity():
    paths=list((REPO/'benchmark/repair').glob('*.py'))
    paths += [REPO/'benchmark'/name for name in ('build.py','run.py','summary.lua')]
    return {str(p.relative_to(REPO)):builds.sha(p) for p in sorted(paths)}


def stable_wrk(tool):
    return {key:tool[key] for key in ('sha256','libraries','version','version_exit')}


def validate_manifest(path,label):
    manifest=bench.validate_manifest(path,label)
    if label=='D':
        bench.demand(manifest.get('base_commit')==BASE and manifest.get('workspace_hashes')==workspace_hashes(),'candidate workspace identity drift')
    return manifest


def budget_seconds(root):
    return sum(json.loads(p.read_text()).get('wall_seconds',0) for p in root.rglob('run.json'))


def require_mechanism_budget(root):
    records=[json.loads(path.read_text()) for path in root.rglob('run.json')]
    count=sum(record.get('kind') in ('trace','experiment') for record in records)
    bench.demand(count<=2,'two mechanism group limit')


def log_bytes(root):
    return sum(p.stat().st_size for p in root.rglob('*') if p.is_file() and p.suffix in ('.stdout','.stderr','.trace'))


def guard(root,prior,started):
    bench.demand(log_bytes(root)<=bench.LOG_LIMIT,'role log/trace budget')
    bench.demand(prior+time.monotonic()-started<=1800,'role 30 minute budget')


def new_output(role,path):
    output=pathlib.Path(path).resolve()
    bench.demand(output.parent==role_root(role).resolve() and output.name.startswith('run-'),'role run- output required')
    bench.demand(not output.exists(),'new output required')
    output.mkdir(parents=True)
    return output


def build_all(role):
    root=role_root(role)
    for label in ('C','D'):
        builds.build(label,root/label)
        if label=='D':
            path=root/label/'manifest.json'
            manifest=json.loads(path.read_text())
            manifest.update(base_commit=BASE,identity_kind='uncommitted-workspace-snapshot',workspace_hashes=workspace_hashes())
            bench.save(path,manifest)
    bench.save(root/'candidate-spec.json',{'base_commit':BASE,'archive_sha256':hashlib.sha256(candidate_archive()).hexdigest(),'workspace_hashes':workspace_hashes()})


def summarize(rows):
    bench.demand(len(rows)==12 and all(r['status']=='valid' for r in rows),'incomplete/invalid suite')
    groups={}
    for label in ('C','D'):
        for size in (1024,1048576):
            group=[r for r in rows if r['label']==label and r['payload']['size']==size]
            bench.demand(len(group)==3,'three original samples required')
            values=[r['measurement']['qps'] for r in group]
            median=statistics.median(values)
            span=(max(values)-min(values))/median
            groups[f'{label}-{size}']={'qps_samples':values,'qps_median':median,'span':span,'noisy':span>.2,'median_of_run_p99_ms':statistics.median(r['measurement']['latency_ms']['p99'] for r in group)}
    ratios={}
    for size in (1024,1048576):
        c,d=groups[f'C-{size}'],groups[f'D-{size}']
        ratios[str(size)]={'qps':d['qps_median']/c['qps_median'],'p99':d['median_of_run_p99_ms']/c['median_of_run_p99_ms']}
    return {'groups':groups,'D_over_C':ratios}


def run_suite(args):
    output=new_output(args.role,args.output)
    role=role_root(args.role)
    started=time.monotonic();prior=budget_seconds(role)
    result={'kind':'formal','status':'invalid','started_utc':bench.utc(),'samples':[],'tools':tool_identity(),'prior_seconds':prior}
    previous_guard=bench.log_guard
    bench.log_guard=lambda _output:guard(role,prior,started)
    try:
        result['environment']=bench.environment(output)
        guard(role,prior,started)
        manifests={label:validate_manifest(role/label/'manifest.json',label) for label in ('C','D')}
        result['builds']=manifests
        result['wrk']=bench.validate_tool(args.wrk)
        root=output/'root';root.mkdir()
        payloads={size:bench.fixture(root,size) for size in (1024,1048576)}
        plan=[(r,s,label) for r in (1,2,3) for s in ((1024,1048576) if r%2 else (1048576,1024)) for label in (('C','D') if r%2 else ('D','C'))]
        result['order']=plan
        bench.save(output/'run.json',result)
        for index,(round_no,size,label) in enumerate(plan,1):
            row=bench.run_sample(manifests[label],args.wrk,root,payloads[size],output/f'sample-{index:02d}-{label}-{size}',output,min(started+600,started+1800-prior))
            row['round']=round_no
            result['samples'].append(row)
            bench.save(output/'run.json',result)
            print(index,label,size,round(row['measurement']['qps'],3),flush=True)
        for label in ('C','D'):
            bench.demand(validate_manifest(role/label/'manifest.json',label)==manifests[label],'build manifest identity drift')
        bench.demand(stable_wrk(bench.validate_tool(args.wrk))==stable_wrk(result['wrk']),'wrk/library identity drift')
        bench.demand(tool_identity()==result['tools'],'script identity drift')
        guard(role,prior,started)
        observed=summarize(result['samples'])
        result['observed_comparison']=observed
        bench.demand(not any(g['noisy'] for g in observed['groups'].values()),'noisy suite requires Leader decision')
        small,large=observed['D_over_C']['1024'],observed['D_over_C']['1048576']
        bench.demand(small['qps']>=10 and small['p99']<=.25,'small response threshold failed')
        bench.demand(large['qps']>=.90 and large['p99']<=1.25,'large response protection failed')
        result['summary']=observed
        result['status']='valid'
        return 0
    except (Exception,KeyboardInterrupt) as error:
        result['error']=repr(error);print(result['error'],file=sys.stderr);return 1
    finally:
        result['wall_seconds']=time.monotonic()-started
        result['ended_utc']=bench.utc()
        result['log_bytes']=log_bytes(role)
        bench.save(output/'run.json',result)
        if (output/'root').exists():shutil.rmtree(output/'root')
        bench.log_guard=previous_guard


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--role',choices=('builder','reviewer'),required=True)
    sub=parser.add_subparsers(dest='action',required=True)
    sub.add_parser('build')
    run=sub.add_parser('run');run.add_argument('--wrk',required=True);run.add_argument('--output',required=True)
    args=parser.parse_args()
    if args.action=='build':build_all(args.role);return 0
    return run_suite(args)

if __name__=='__main__':raise SystemExit(main())
