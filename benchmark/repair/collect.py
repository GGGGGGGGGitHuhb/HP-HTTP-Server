#!/usr/bin/env python3
"""Preserve S2 original records, compact resource summaries and log hashes."""
import argparse
import json
import pathlib
import shutil
import repair


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--role',choices=('builder','reviewer'),required=True)
    parser.add_argument('--output',required=True)
    args=parser.parse_args()
    output=pathlib.Path(args.output).resolve()
    repair.bench.demand(output.is_relative_to(repair.REPO/'benchmark/results/V0.5.1/S2') and not output.exists(),'new S2 result directory required')
    output.mkdir(parents=True)
    root=repair.role_root(args.role)
    summary={'role':args.role,'runs':{},'log_hashes':{},'dynamic_seconds':0}
    for path in sorted(root.glob('run-*/run.json')):
        run=json.loads(path.read_text());name=path.parent.name
        shutil.copyfile(path,output/(name+'.json'))
        summary['runs'][name]={'status':run['status'],'kind':run['kind'],'wall_seconds':run['wall_seconds'],'error':run.get('error'),'summary':run.get('summary'),'observed_comparison':run.get('observed_comparison'),'body_wait_medians_ms':run.get('body_wait_medians_ms'),'sample_log_bytes':{d.name:repair.log_bytes(d) for d in path.parent.glob('sample-*') if d.is_dir()}}
        summary['dynamic_seconds']+=run['wall_seconds']
        for log in path.parent.rglob('*'):
            if log.is_file() and log.suffix in ('.stdout','.stderr','.trace'):
                summary['log_hashes'][str(log.relative_to(root))]={'bytes':log.stat().st_size,'sha256':repair.builds.sha(log)}
        for trace in path.parent.glob('*.trace'):shutil.copyfile(trace,output/(name+'-'+trace.name))
    repair.bench.save(output/'summary.json',summary)
    print('runs',len(summary['runs']),'dynamic_seconds',round(summary['dynamic_seconds'],3))

if __name__=='__main__':main()
