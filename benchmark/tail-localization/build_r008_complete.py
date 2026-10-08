"""Sealed R008 compilation/format/pure units; no socket probes or HTTP samples."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT=Path(__file__).resolve().parents[2]
TOOLS=Path(__file__).resolve().parent
ROLE=ROOT/'.cache/v0.5.1-s4/builder'


def patch_module(name):
    spec=importlib.util.spec_from_file_location(name,TOOLS/name)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    return module


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();output=args.output.absolute()
    if output!=ROLE/'run-r008-build-002' or not output.is_dir():raise ValueError('exact admission required')
    for item in json.loads((ROLE/'cache/r008-build-seal-002/inputs.json').read_text()):
        if hashlib.sha256(Path(item['path']).read_bytes()).hexdigest()!=item['sha256']:raise ValueError('sealed compile input drift')
    commands=[]
    def execute(label,argv,cwd=ROOT,env=None):
        commands.append(dict(label=label,argv=argv,cwd=str(cwd)))
        (output/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        with (output/(label+'.stdout')).open('wb') as stdout,(output/(label+'.stderr')).open('wb') as stderr:
            subprocess.run(argv,cwd=cwd,env=env,stdout=stdout,stderr=stderr,check=True)
    server=output/'server';client=output/'client' 
    patch_module('server/patch_server_v3.py').prepare(ROLE/'E-S3/source',server,TOOLS)
    patch_module('client/patch_client_v3.py').prepare(ROLE/'wrk-source/wrk-4.1.0',client,TOOLS)
    names=['src/base/ObservedRuntime.cpp','include/tail_localization/ObservedRuntime.h','include/tail_localization/TailWire.h','include/tail_localization/StartupEvidence.h','include/tail_localization/ClockEvidence.h','include/tail_localization/ObserverBuffer.h','app/main.cpp','src/base/AsyncLogger.cpp','src/net/TcpServer.cpp','include/net/TcpConnection.h','src/net/TcpConnection.cpp','src/net/ConnectionIo.cpp','app/HttpConnectionHandler.cpp']
    (output/'format-files.txt').write_text('\n'.join(names)+'\n')
    version=subprocess.check_output(['/usr/bin/clang-format-18','--version'],text=True)
    if '18.1.3' not in version:raise ValueError('clang-format version differs')
    execute('format',['/usr/bin/clang-format-18','-i',*map(lambda name:str(server/name),names)])
    execute('format-check',['/usr/bin/clang-format-18','--dry-run','--Werror',*map(lambda name:str(server/name),names)])
    for kind in ('Debug','Release'):
        build=server/'.test-tmp'/kind.lower()
        execute(kind.lower()+'-configure',['/usr/bin/cmake','-S',str(server),'-B',str(build),'-DCMAKE_BUILD_TYPE='+kind,'-DBUILD_TESTING='+('ON' if kind=='Debug' else 'OFF')])
        if kind=='Debug':
            config=build/'CTestTestfile.cmake';before=config.read_bytes()
            old=str(build/'test-tmp').encode();new=str(server/'.test-tmp').encode()
            if old not in before:raise ValueError('generated TMP route seam missing')
            (build/'CTestTestfile.before-route.cmake').write_bytes(before);config.write_bytes(before.replace(old,new))
        execute(kind.lower()+'-build',['/usr/bin/cmake','--build',str(build),'-j4'])
    runtime=ROOT/'.cache/v0.5-s4/tools/root/usr/lib/x86_64-linux-gnu/libluajit-5.1.so.2.1.1703358377'
    development=ROLE/'client-build-inputs/root/usr'
    environment=os.environ.copy();environment['PATH']=str(development/'bin')+':'+environment['PATH']
    environment['LD_LIBRARY_PATH']=str(runtime.parent)
    environment['LUA_PATH']=str(ROOT/'.cache/v0.5-s4/tools/root/usr/share/luajit-2.1/?.lua')+';;'
    execute('client-build',['/usr/bin/make','-j4','VER=4.1.0','PKG_CONFIG=/usr/bin/true','WITH_LUAJIT='+str(development),'WITH_OPENSSL=/usr','CPPFLAGS=-I'+str(development/'include/luajit-2.1'),'LIBS='+str(runtime)+' -lpthread -lm -lssl -lcrypto -ldl'],client,environment)
    unit=output/'startup-unit.cpp';shutil.copyfile(TOOLS/'r008_startup_unit.cpp',unit)
    (output/'unit-format-files.txt').write_text('startup-unit.cpp\n')
    execute('unit-format',['/usr/bin/clang-format-18','-i',str(unit)])
    execute('unit-format-check',['/usr/bin/clang-format-18','--dry-run','--Werror',str(unit)])
    execute('unit-build',['/usr/bin/c++','-std=c++20','-g','-O1','-fno-omit-frame-pointer','-fsanitize=address,undefined','-I'+str(server/'include'),'-I'+str(server/'include/tail_localization'),str(unit),str(server/'src/base/ObservedRuntime.cpp'),'-Wl,--wrap=getsockname','-Wl,--wrap=getpeername','-o',str(output/'startup-unit')])
    execute('protocol-pure',['/usr/bin/python3','-m','unittest','discover','-s',str(TOOLS),'-p','test_r008_protocol.py','-v'])
    print(json.dumps(dict(status='compiled-formatted-protocol-pure',remaining=['sanitizer unit execution','Debug9CTest','four real steps','offline association'])))


if __name__=='__main__':main()
