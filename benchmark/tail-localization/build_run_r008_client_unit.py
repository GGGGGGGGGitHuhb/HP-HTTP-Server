"""Compile and execute actual patched wrk callbacks with transport/epoll mocks."""
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
    if output!=ROLE/'run-r008-client-unit-001' or not output.is_dir():raise ValueError('exact pure-unit reservation required')
    for item in json.loads((ROLE/'cache/r008-client-unit-seal-001/inputs.json').read_text()):
        if hashlib.sha256(Path(item['path']).read_bytes()).hexdigest()!=item['sha256']:raise ValueError('client-unit input drift')
    client=ROLE/'run-r008-build-003/client'
    unit=output/'client-unit.c';shutil.copyfile(TOOLS/'r008_client_callbacks_unit.c',unit)
    objects=sorted(str(path) for path in (client/'obj').glob('*.o') if path.name not in ('wrk.o','ClientObserver.o'))
    runtime=ROOT/'.cache/v0.5-s4/tools/root/usr/lib/x86_64-linux-gnu/libluajit-5.1.so.2.1.1703358377'
    headers=ROLE/'client-build-inputs/root/usr/include/luajit-2.1'
    command=['/usr/bin/cc','-std=c99','-g','-O1','-D_GNU_SOURCE','-fno-omit-frame-pointer','-fsanitize=address,undefined','-DHP_S4_CLIENT_SOURCE="'+str(client/'src/wrk.c')+'"','-I'+str(client/'src'),'-I'+str(headers),str(unit),str(client/'src/ClientObserver.c'),*objects,str(runtime),'-lpthread','-lm','-lssl','-lcrypto','-ldl','-Wl,--wrap=aeCreateFileEvent','-Wl,--wrap=aeDeleteFileEvent','-o',str(output/'client-unit')]
    (output/'compile-command.json').write_text(json.dumps(command,indent=2)+'\n')
    with (output/'compile.stdout').open('wb') as stdout,(output/'compile.stderr').open('wb') as stderr:
        subprocess.run(command,stdout=stdout,stderr=stderr,check=True)
    env=os.environ.copy();env.update(LD_LIBRARY_PATH=str(runtime.parent),ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    fixture=output/'fixture';fixture.mkdir()
    with (output/'unit.stdout').open('wb') as stdout,(output/'unit.stderr').open('wb') as stderr:
        subprocess.run([str(output/'client-unit'),str(fixture)],stdout=stdout,stderr=stderr,env=env,check=True)
    print(json.dumps(dict(status='valid',covered=['actual socket_writeable short write/RETRY/EAGAIN/sequence continuity','actual response_complete per-connection16 stop without extra WRITE','second connection not completed by global count','GO-before-write/response rejected','ASan/UBSan/LSan'],live_socket_probes=0)))


if __name__=='__main__':main()
