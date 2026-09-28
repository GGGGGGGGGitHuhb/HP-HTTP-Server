#!/usr/bin/env python3
"""Approved first intermediate version, measured separately from formal A/B/C."""
import argparse
import statistics
import time
import diagnose as diag

# 2026-09-28: peeled v0.5-s1; label alone never supplies identity.
diag.builds.COMMITS['S1']='70b866bddbe7b4219037a93d23bde19702399d73'
diag.builds.TREES['S1']='71c5a2bdbc0e91457e97bd5ae48d9f7858c7634b'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--role',choices=('builder','reviewer'),required=True)
    parser.add_argument('--build',action='store_true')
    parser.add_argument('--wrk')
    parser.add_argument('--output')
    args=parser.parse_args()
    role=diag.role_root(args.role)
    if args.build:
        diag.builds.build('S1',role/'S1');return 0
    output=diag.checked_output(args.role,args.output)
    began=time.monotonic();prior=diag.accumulated_seconds(role)
    result=dict(kind='intermediate',status='invalid',samples=[],tools=diag.identities(),
                intermediate_sha256=diag.builds.sha(__file__),started_utc=diag.legacy.utc(),prior_dynamic_seconds=prior)
    previous_guard=diag.legacy.log_guard
    def guard(_output):
        previous_guard(role)
        diag.legacy.demand(sum(p.stat().st_size for p in role.rglob('*.trace'))+diag.legacy.log_bytes(role)<=diag.legacy.LOG_LIMIT,'cumulative trace/log budget')
        diag.legacy.demand(prior+time.monotonic()-began<=1800,'role dynamic budget')
    diag.legacy.log_guard=guard
    try:
        result['environment']=diag.legacy.environment(output)
        manifest=diag.legacy.validate_manifest(role/'S1/manifest.json','S1')
        result['build']=manifest
        result['wrk']=diag.legacy.validate_tool(args.wrk)
        root=output/'root';root.mkdir();payload=diag.legacy.fixture(root,1024)
        for index in range(3):
            row=diag.legacy.run_sample(manifest,args.wrk,root,payload,output/f'sample-{index+1}',output,min(began+600,began+1800-prior))
            result['samples'].append(row)
            diag.legacy.save(output/'run.json',result)
            print('S1',index+1,row['measurement']['qps'],flush=True)
        diag.legacy.validate_manifest(role/'S1/manifest.json','S1')
        final_tool=diag.legacy.validate_tool(args.wrk)
        diag.legacy.demand(diag.stable_tool_identity(final_tool)==diag.stable_tool_identity(result['wrk']),'wrk identity drift')
        diag.legacy.demand(diag.identities()==result['tools'] and diag.builds.sha(__file__)==result['intermediate_sha256'],'diagnostic tool identity drift')
        guard(output)
        result['qps_median']=statistics.median(r['measurement']['qps'] for r in result['samples'])
        result['status']='valid'
    except (Exception,KeyboardInterrupt) as error:result['error']=repr(error)
    finally:
        result['wall_seconds']=time.monotonic()-began
        result['ended_utc']=diag.legacy.utc()
        diag.legacy.save(output/'run.json',result)
        if (output/'root').exists():diag.shutil.rmtree(output/'root')
        diag.legacy.log_guard=previous_guard
    return 0 if result['status']=='valid' else 1

if __name__=='__main__':raise SystemExit(main())
