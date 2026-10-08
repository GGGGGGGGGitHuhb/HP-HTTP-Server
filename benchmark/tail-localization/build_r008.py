"""One admitted R008 compile checkpoint; no live socket or performance probes."""
import argparse
import importlib.util
import json
import hashlib
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
TOOLS = Path(__file__).resolve().parent
ROLE = ROOT / '.cache/v0.5.1-s4/builder'


def load_patch(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / name)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.absolute()
    if output != ROLE / 'run-r008-build-001' or not output.is_dir():
        raise ValueError('exact admitted build output required')
    sealed=json.loads((ROLE/'cache/r008-build-seal-001/inputs.json').read_text())
    for item in sealed:
        if hashlib.sha256(Path(item['path']).read_bytes()).hexdigest()!=item['sha256']:
            raise ValueError('build input drift: '+item['path'])
    for name in ('build_r008.py','server/patch_server_v3.py','client/patch_client_v3.py','wire_types_v3.py','budget_v8.py','localize_v8.py'):
        path=TOOLS/name
        compile(path.read_text(),str(path),'exec')
    with (output/'abi.stdout').open('wb') as stdout,(output/'abi.stderr').open('wb') as stderr:
        subprocess.run(['/usr/bin/cc','-std=c11','-fsyntax-only','-I'+str(TOOLS/'common'),str(TOOLS/'r008_abi_check.c')],stdout=stdout,stderr=stderr,check=True)
    spec=importlib.util.spec_from_file_location('wire_r008',TOOLS/'wire_types_v3.py')
    wire=importlib.util.module_from_spec(spec);spec.loader.exec_module(wire)
    import ctypes
    if [ctypes.sizeof(item) for item in (wire.Control,wire.Connection,wire.Thread,wire.Failure,wire.Registration)] != [25192,64,32,104,32]:
        raise ValueError('Python startup ABI mismatch')
    if wire.Control.phase.offset!=16864 or wire.Control.firstFailure.offset!=16896 or wire.Control.serverRegistrations.offset!=17000 or wire.Failure.connection.offset!=32:
        raise ValueError('Python startup ABI offset mismatch')
    load_patch('server/patch_server_v3.py').prepare(ROLE/'E-S3/source', output/'server', TOOLS)
    load_patch('client/patch_client_v3.py').prepare(ROLE/'wrk-source/wrk-4.1.0', output/'client', TOOLS)
    server = output/'server'
    commands = [
        (ROOT, ['/usr/bin/cmake','-S',str(server),'-B',str(output/'debug'),'-DCMAKE_BUILD_TYPE=Debug','-DBUILD_TESTING=ON']),
        (ROOT, ['/usr/bin/cmake','--build',str(output/'debug'),'-j4']),
    ]
    for index,(cwd,command) in enumerate(commands):
        with (output/f'compile-{index}.stdout').open('wb') as stdout, (output/f'compile-{index}.stderr').open('wb') as stderr:
            subprocess.run(command,cwd=cwd,stdout=stdout,stderr=stderr,check=True)
    print(json.dumps(dict(status='compiled-debug',output=str(output),remaining=['client compile','Release binary','format','pure cases','sanitizer','real four steps'])))


if __name__ == '__main__':
    main()
