"""Current 16 pure obligations plus a freshly compiled actual client capacity unit."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT=Path(__file__).resolve().parents[2]
TOOLS=Path(__file__).resolve().parent


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--role',choices=('builder','reviewer'),required=True);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();output=args.output.absolute();role=ROOT/'.cache/v0.5.1-s4'/args.role
    expected_run={'builder':'run-r008-m2-pure-002','reviewer':'run-r008-m2-pure-001'}[args.role]
    if output!=role/expected_run or not output.is_dir() or any(path.is_symlink() for path in (output,*output.parents)):raise ValueError('exact own role pure output required')
    ledger=json.loads((role/'ledger.json').read_text());runs=[item for item in ledger['runs'] if item['run_id']==output.name]
    if len(runs)!=1 or runs[0]['status']!='running' or runs[0]['kind']!='selfcheck' or runs[0]['reserved_seconds']!=20:raise ValueError('single bounded pure reservation required')
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
