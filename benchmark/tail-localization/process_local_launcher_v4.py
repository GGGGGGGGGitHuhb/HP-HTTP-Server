"""Process-local A/B entry; inherits the admitted outer deadline, never root resources."""
import argparse
import json
import os
from pathlib import Path
import time
from observed_sample_v4 import sample


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--role',choices=('builder','reviewer'),default='builder')
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--manifest',required=True)
    parser.add_argument('--requests-per-connection',type=int,choices=(1,16),required=True)
    args=parser.parse_args()
    role=Path(__file__).resolve().parents[2]/'.cache/v0.5.1-s4'/args.role
    expected='run-r008-link-once' if args.requests_per_connection==1 else 'run-r008-link-repeat'
    if args.output.absolute()!=role/expected or os.getuid()!=1000:
        raise ValueError('exact power A/B admission required')
    ledger=json.loads((role/'ledger.json').read_text())
    entries=[item for item in ledger['runs'] if item['run_id']==expected]
    if len(entries)!=1 or entries[0]['status']!='running' or entries[0]['kind']!='r008_link' or entries[0]['reserved_seconds']!=30:
        raise ValueError('outer A/B reservation missing')
    args.total_deadline=entries[0]['start_monotonic']+30
    args.work_deadline=args.total_deadline-7
    if time.monotonic()>=args.work_deadline:
        raise TimeoutError('A/B outer deadline elapsed')
    args.connections=2;args.warmup=0;args.duration=1;args.detailed=True
    args.identity_scope='process-local';args.marker_fd=None;args.root_socket_fd=None
    return sample(args)


if __name__=='__main__':
    raise SystemExit(main())
