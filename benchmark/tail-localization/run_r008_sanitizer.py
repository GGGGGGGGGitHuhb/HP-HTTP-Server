"""Admitted R008 mock startup units with ASan/UBSan/LSan, no socket creation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[2]
ROLE=ROOT/'.cache/v0.5.1-s4/builder'
SCENARIOS=('worker-missing','worker-range','limit','duplicate','different-owner','closed-before-go','request-before-go','first-cause')


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path,required=True);parser.add_argument('--binary-sha256',required=True)
    args=parser.parse_args();output=args.output.absolute();binary=ROLE/'run-r008-build-003/startup-unit'
    if output!=ROLE/'run-r008-sanitizer-001' or not output.is_dir():raise ValueError('exact sanitizer reservation required')
    if hashlib.sha256(binary.read_bytes()).hexdigest()!=args.binary_sha256:raise ValueError('sanitizer binary drift')
    env=os.environ.copy();env.update(ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    results=[]
    for scenario in SCENARIOS:
        directory=output/scenario;directory.mkdir()
        command=[str(binary),scenario,str(directory)]
        with (output/(scenario+'.stdout')).open('wb') as stdout,(output/(scenario+'.stderr')).open('wb') as stderr:
            result=subprocess.run(command,stdout=stdout,stderr=stderr,env=env,check=False)
        results.append(dict(scenario=scenario,command=command,exit_code=result.returncode))
        (output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
        if result.returncode:return result.returncode
    print(json.dumps(dict(status='valid',cases=len(results),sanitizers=['ASan','UBSan','LSan'])))
    return 0


if __name__=='__main__':raise SystemExit(main())
