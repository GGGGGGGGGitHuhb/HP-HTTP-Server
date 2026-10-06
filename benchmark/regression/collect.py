#!/usr/bin/env python3
"""Publish compact immutable records and original wrk stdout/stderr, never builds."""
import argparse
import pathlib
import shutil
import budget
import identity
from identity import legacy


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--role',choices=('builder','reviewer'),required=True)
    parser.add_argument('--output',required=True)
    args=parser.parse_args()
    output=pathlib.Path(args.output).resolve()
    base=identity.REPO/'benchmark/results/V0.5.1/S3'
    legacy.demand(output.is_relative_to(base) and not output.exists(),'new S3 result directory required')
    output.mkdir(parents=True)
    root=identity.role_root(args.role)
    summary={'role':args.role,'runs':{},'log_hashes':{},'dynamic_seconds':budget.charged(root,budget.load(root/'ledger.json'))}
    for path in sorted(root.glob('run-*/run.json')):
        record=budget.load(path)
        name=path.parent.name
        target=output/name;target.mkdir()
        shutil.copyfile(path,target/'archive-run.json')
        summary['runs'][name]={k:record.get(k) for k in ('kind','status','wall_seconds','error','performance_acceptance','observed_comparison')}
        for log in sorted(path.parent.rglob('*')):
            if not log.is_file():continue
            if log.suffix in ('.stdout','.stderr'):
                summary['log_hashes'][str(log.relative_to(root))]={'bytes':log.stat().st_size,'sha256':legacy.sha(log)}
            # Server stdout/stderr can be huge; retain hashes and sizes, and all wrk outputs for independent arithmetic.
            if log.name in ('sample.json','schedule.json') or log.suffix=='.json' and log.name.endswith(('.cleanup.json','.process.json')) or log.suffix in ('.stdout','.stderr') and log.name not in ('server.stdout','server.stderr'):
                dest=target/log.relative_to(path.parent);dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(log,dest)
    shutil.copyfile(root/'ledger.json',output/'ledger.json')
    legacy.save(output/'summary.json',summary)
    print('runs',len(summary['runs']),'dynamic_seconds',round(summary['dynamic_seconds'],3))

if __name__=='__main__':main()
