"""Current 16 pure obligations plus a freshly compiled actual client capacity unit."""
import argparse
import json
import math
import os
from pathlib import Path
import shutil
import subprocess

ROOT=Path(__file__).resolve().parents[2]
TOOLS=Path(__file__).resolve().parent


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--role',choices=('builder','reviewer'),required=True);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();output=args.output.absolute();role=ROOT/'.cache/v0.5.1-s4'/args.role
    allowed={'builder':('run-r008-m2-pure-002','run-r008-m2-pure-003','run-r008-m2-pure-004'),'reviewer':('run-r008-m2-pure-001',)}[args.role]
    if output.name not in allowed or output!=role/output.name or not output.is_dir() or any(path.is_symlink() for path in (output,*output.parents)):raise ValueError('exact own role pure output required')
    ledger=json.loads((role/'ledger.json').read_text());records=ledger['runs']
    current=[item for item in records if item['run_id']==output.name]
    running=[item for item in records if item['status']=='running']
    if len(current)!=1 or len(running)!=1 or running[0] is not current[0] or current[0]['kind']!='selfcheck' or current[0]['reserved_seconds']!=20:raise ValueError('single bounded pure reservation required')
    for item in records:
        if item is current[0]:continue
        charge=item.get('charged_seconds')
        if item.get('status') not in ('valid','invalid') or isinstance(charge,bool) or not isinstance(charge,(int,float)) or not math.isfinite(charge) or charge<0 or item.get('accounting_errors')!=[] or item.get('byte_classification_status')!='verified':raise ValueError('unsettled or unknown accounting record')
    if args.role=='builder':
        window=[item for item in records if item['run_id'] in allowed]
        index=allowed.index(output.name)
        if len(window)>3 or [item['run_id'] for item in window]!=list(allowed[:index+1]):raise ValueError('window duplicate, skipped, or out-of-order run')
        earlier=window[:-1]
        if any(item['status']!='invalid' or item['kind']!='selfcheck' or item['reserved_seconds']!=20 for item in earlier):raise ValueError('window previously succeeded or was not a settled failed attempt')
        if sum(item['charged_seconds'] for item in earlier)+20>60:raise ValueError('window actual charges plus reservation exceed sixty seconds')
    commands=[]
    def execute(label,argv,env=None):
        commands.append(dict(label=label,argv=argv));(output/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        with (output/(label+'.stdout')).open('xb') as out,(output/(label+'.stderr')).open('xb') as err:
            subprocess.run(argv,stdout=out,stderr=err,env=env,check=True)
    execute('m2-pure',['/usr/bin/python3',str(TOOLS/'run_r009_m2_pure.py')])
    source=output/'client-unit-source';source.mkdir()
    for origin,name in [('client/ClientObserverV3.c','ClientObserver.c'),('client/ClientObserverV3.h','ClientObserver.h'),('common/TailWireV3.h','TailWire.h'),('common/StartupEvidence.h','StartupEvidence.h'),('common/ClockEvidence.h','ClockEvidence.h'),('r009_client_buffer_unit.c','unit.c')]:shutil.copyfile(TOOLS/origin,source/name)
    execute('unit-format',['/usr/bin/clang-format-18','-i',str(source/'unit.c')])
    execute('unit-format-check',['/usr/bin/clang-format-18','--dry-run','--Werror',str(source/'unit.c')])
    execute('client-buffer-compile',['/usr/bin/cc','-std=c99','-D_GNU_SOURCE','-g','-O1','-fno-omit-frame-pointer','-fsanitize=address,undefined','-I'+str(source),str(source/'unit.c'),str(source/'ClientObserver.c'),'-o',str(output/'client-buffer-unit')])
    fixture=output/'client-buffer-fixture';fixture.mkdir()
    environment=os.environ.copy();environment.update(ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    execute('client-buffer-sanitizer',[str(output/'client-buffer-unit'),str(fixture)],environment)
    print(json.dumps(dict(status='m2-pure16-current-client-capacity-sanitizer-valid',server_buffer='same-source-sha99a09f13d2ef4c02a9ad93b1a608cd77c5afea7f95338b72a0134a3a87c98a5a')))

if __name__=='__main__':main()
