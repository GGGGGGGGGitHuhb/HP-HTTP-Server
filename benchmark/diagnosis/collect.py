#!/usr/bin/env python3
"""Copy compact original JSON and hashes; keep large raw logs in the role root."""
import argparse
import json
import pathlib
import shutil
import statistics
import diagnose as diag


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--role',choices=('builder','reviewer'),required=True)
    parser.add_argument('--output',required=True)
    args=parser.parse_args()
    output=pathlib.Path(args.output).resolve()
    allowed=diag.REPO/'benchmark/results/V0.5.1'
    diag.legacy.demand(output.is_relative_to(allowed) and not output.exists(),'new result directory required')
    output.mkdir(parents=True)
    role=diag.role_root(args.role)
    summary=dict(role=args.role,runs={},total_dynamic_seconds=0,raw_log_hashes={})
    for record in sorted(role.glob('run-*/run.json')):
        run=json.loads(record.read_text())
        name=record.parent.name
        shutil.copyfile(record,output/(name+'.json'))
        metrics=dict(status=run['status'],kind=run.get('kind'),wall_seconds=run.get('wall_seconds',0),error=run.get('error'))
        summary['total_dynamic_seconds']+=metrics['wall_seconds']
        for key in ('summary','qps_median'):
            if key in run:metrics[key]=run[key]
        if 'series' in run:
            metrics['body_wait_medians_ms']=[statistics.median(r['body_wait_ms'] for r in series) for series in run['series']]
            metrics['body_wait_maxima_ms']=[max(r['body_wait_ms'] for r in series) for series in run['series']]
        metrics['sample_log_bytes']={p.name:diag.legacy.log_bytes(p) for p in record.parent.glob('sample-*') if p.is_dir()}
        summary['runs'][name]=metrics
        for p in record.parent.rglob('*'):
            if p.is_file() and p.suffix in ('.stdout','.stderr','.trace'):
                summary['raw_log_hashes'][str(p.relative_to(role))]=dict(bytes=p.stat().st_size,sha256=diag.builds.sha(p))
        for trace in record.parent.glob('*.trace'):
            shutil.copyfile(trace,output/(name+'-'+trace.name))
    diag.legacy.save(output/'summary.json',summary)
    print('runs',len(summary['runs']),'dynamic_seconds',round(summary['total_dynamic_seconds'],2))

if __name__=='__main__':main()
