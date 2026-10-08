"""Continue only failed client build and unexecuted unit/protocol work in a new run."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT=Path(__file__).resolve().parents[2]
TOOLS=Path(__file__).resolve().parent
ROLE=ROOT/'.cache/v0.5.1-s4/builder'


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();output=args.output.absolute()
    if output!=ROLE/'run-r008-build-003' or not output.is_dir():raise ValueError('exact continuation reservation required')
    seal=json.loads((ROLE/'cache/r008-build-seal-003/inputs.json').read_text())
    for item in seal:
        if hashlib.sha256(Path(item['path']).read_bytes()).hexdigest()!=item['sha256']:raise ValueError('continuation source drift')
    previous=ROLE/'run-r008-build-002'
    shutil.copytree(previous/'client',output/'client')
    commands=[]
    def execute(label,command,cwd=ROOT,env=None):
        commands.append(dict(label=label,command=command,cwd=str(cwd),environment={key:env.get(key) for key in ('PATH','LD_LIBRARY_PATH','LUA_PATH')} if env else None))
        (output/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        with (output/(label+'.stdout')).open('wb') as stdout,(output/(label+'.stderr')).open('wb') as stderr:
            subprocess.run(command,cwd=cwd,env=env,stdout=stdout,stderr=stderr,check=True)
    runtime=ROOT/'.cache/v0.5-s4/tools/root/usr/lib/x86_64-linux-gnu/libluajit-5.1.so.2.1.1703358377'
    development=ROLE/'client-build-inputs/root/usr'
    env=os.environ.copy();env.update(PATH=str(development/'bin')+':/usr/bin:/bin',LD_LIBRARY_PATH=str(runtime.parent),LUA_PATH=str(ROOT/'.cache/v0.5-s4/tools/root/usr/share/luajit-2.1/?.lua')+';;')
    execute('client-build',['/usr/bin/make','-j4','VER=4.1.0','PKG_CONFIG=/usr/bin/true','WITH_LUAJIT='+str(development),'WITH_OPENSSL=/usr','CPPFLAGS=-I'+str(development/'include/luajit-2.1'),'LIBS='+str(runtime)+' -lpthread -lm -lssl -lcrypto -ldl'],output/'client',env)
    unit=output/'startup-unit.cpp';shutil.copyfile(TOOLS/'r008_startup_unit.cpp',unit)
    version=subprocess.check_output(['/usr/bin/clang-format-18','--version'],text=True)
    if '18.1.3' not in version:raise ValueError('formatter version drift')
    execute('unit-format',['/usr/bin/clang-format-18','-i',str(unit)])
    execute('unit-format-check',['/usr/bin/clang-format-18','--dry-run','--Werror',str(unit)])
    server=previous/'server'
    execute('unit-build',['/usr/bin/c++','-std=c++20','-g','-O1','-fno-omit-frame-pointer','-fsanitize=address,undefined','-I'+str(server/'include'),'-I'+str(server/'include/tail_localization'),str(unit),str(server/'src/base/ObservedRuntime.cpp'),'-Wl,--wrap=getsockname','-Wl,--wrap=getpeername','-o',str(output/'startup-unit')])
    execute('protocol-pure',['/usr/bin/python3','-m','unittest','discover','-s',str(TOOLS),'-p','test_r008_protocol.py','-v'])
    print(json.dumps(dict(status='client-unit-compiled-protocol-pure',remaining=['sanitizer execution','Debug9CTest','four recovery samples','association'])))


if __name__=='__main__':main()
