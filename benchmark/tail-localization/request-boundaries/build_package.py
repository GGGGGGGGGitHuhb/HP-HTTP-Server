"""R018 one relocated build, exact role-owned E reuse, explicit receipt protocol."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
sys.dont_write_bytecode = True
ROOT = Path(__file__).absolute().parent
for parent in (ROOT,*ROOT.parents):
    if parent.is_symlink():
        raise ValueError('linked package root')
def load_own(name):
    path=ROOT/(name+'.py')
    if path.is_symlink(): raise ValueError('linked package module')
    spec=importlib.util.spec_from_file_location('r018_'+name,path)
    module=importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module
prepare=load_own('prepare_package')
archive=load_own('archive_inputs')

def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--package-root',required=True)
    parser.add_argument('--output-root',required=True)
    parser.add_argument('--server-o-receipt',required=True)
    parser.add_argument('--server-o-receipt-sha256',required=True)
    args=parser.parse_args()
    package=prepare.validate_directory_path(args.package_root)
    if package!=ROOT: raise ValueError('entry package mismatch')
    output=Path(args.output_root).absolute()
    prepare.verify_execution_context(package,output.parent,'run-r018-build-001')
    deadline=min(time.monotonic()+80,float(os.environ['HP_BASELINE_WORK_DEADLINE']))
    if not time.monotonic()<deadline<float(os.environ['HP_BASELINE_CLEANUP_DEADLINE']): raise ValueError('outer deadlines')
    old_receipt=Path(args.server_o_receipt).absolute()
    prepare.validate_directory_path(old_receipt.parent)
    if old_receipt.is_symlink() or digest(old_receipt)!=args.server_o_receipt_sha256: raise ValueError('O receipt drift')
    old=json.loads(old_receipt.read_text())
    if old['schema']!='baseline-v1-build': raise ValueError('O receipt schema')
    old_build=old['builds'][1]
    expected=Path(os.environ['HP_BASELINE_OUTPUT_ROOT']).parents[0]/'run-r015-build-001/build-output'
    if old_receipt!=expected/'build-receipt.json': raise ValueError('O not same role source')
    if Path(old_build['outputRoot'])!=expected/'relocated':raise ValueError('O output not same role relocated build')
    old_binary=Path(old_build['outputRoot'])/'build-E/hp_http_server'
    prepare.validate_directory_path(old_binary.parent)
    if old_binary.is_symlink() or digest(old_binary)!=old_build['serverSha256'] or old_build['serverSourceCommit']!='acda3f92d42a36d0b0554e185bc6f4155b4e5889': raise ValueError('O binary/source drift')
    if not any('-DCMAKE_BUILD_TYPE=Release' in command and '-DBUILD_TESTING=OFF' in command for command in old_build['commands']): raise ValueError('O Release parameters')
    for dependency in old_build['actualLoadedLibraries'][str(old_binary)]['files']:
        prepare.validate_directory_path(Path(dependency['path']).parent)
        if Path(dependency['path']).is_symlink():raise ValueError('O linked DSO')
        if digest(dependency['path'])!=dependency['sha256']: raise ValueError('O DSO drift')
    output.mkdir(exist_ok=False)
    relocated=prepare.prepare_package(package,output/'relocated-package')
    temporary=output/'tmp';temporary.mkdir()
    env={'PATH':'/usr/bin:/bin','HOME':str(temporary),'TMPDIR':str(temporary),'TMP':str(temporary),'TEMP':str(temporary),
         'XDG_CACHE_HOME':str(temporary),'LANG':'C','LC_ALL':'C','LD_LIBRARY_PATH':str(relocated/'runtime'),
         'LUA_PATH':str(relocated/'runtime/lua/?.lua')+';'+str(relocated/'runtime/lua/?/init.lua'),'LUA_CPATH':''}
    commands=[]
    def execute(command,cwd=output):
        remaining=deadline-time.monotonic()
        if remaining<=0: raise TimeoutError('build work deadline')
        index=len(commands);commands.append([str(value) for value in command])
        with (output/f'command-{index}.stdout').open('xb') as stdout,(output/f'command-{index}.stderr').open('xb') as stderr:
            subprocess.run(commands[-1],cwd=cwd,env=env,stdout=stdout,stderr=stderr,timeout=remaining,check=True)
    for tool in archive.TOOLS.values():
        if not Path(tool).is_file(): raise ValueError('missing tool '+tool)
    system_tools={name:{'path':path,'realpath':str(Path(path).resolve(strict=True)),
                        'sha256':digest(Path(path).resolve(strict=True))}
                  for name,path in {**archive.TOOLS,'formatter':'/usr/bin/clang-format-18'}.items()}
    for name in ('cc','cxx','cmake','make','python'):
        execute([archive.TOOLS[name],'--version'])
        versions=(output/f'command-{len(commands)-1}.stdout').read_text().splitlines()
        if not versions:raise ValueError('missing system tool version')
        system_tools[name]['version']=versions[0]
    openssl_headers={str(path.resolve(strict=True)):digest(path.resolve(strict=True)) for base in (Path('/usr/include/openssl'),Path('/usr/include/x86_64-linux-gnu/openssl'))
                     for path in sorted(base.rglob('*.h'))}
    if not openssl_headers:raise ValueError('missing declared OpenSSL development headers')
    source=output/'source-B'
    archive.extract_archive((relocated/'inputs/archive/E-S3.tar').open('rb'),source,deadline)
    patch_paths=['CMakeLists.txt','include/net/TcpConnection.h','src/net/TcpConnection.cpp',
                 'include/net/ConnectionRegistry.h','src/net/ConnectionRegistry.cpp',
                 'include/net/TcpServer.h','src/net/TcpServer.cpp','app/HttpConnectionHandler.cpp']
    server_patch_before={name:digest(source/name) for name in patch_paths}
    execute(['/usr/bin/python3','-I',relocated/'server/patch_server.py',source,relocated/'server'])
    server_patch_after={name:digest(source/name) for name in patch_paths}
    if any(server_patch_before[name]==server_patch_after[name] for name in patch_paths):
        raise ValueError('server patch did not modify every declared source')
    observer_files=[source/'include/net/RequestBoundaryObserver.h',source/'src/net/RequestBoundaryObserver.cpp']
    # Exact patched locations are supplied by the server patch contract.
    for path in observer_files:
        if not path.is_file(): raise ValueError('observer source location mismatch')
    format_files=sorted({*observer_files,source/'src/net/TcpConnection.cpp',source/'include/net/TcpConnection.h',
                         source/'src/net/ConnectionRegistry.cpp',source/'include/net/ConnectionRegistry.h',
                         source/'src/net/TcpServer.cpp',source/'include/net/TcpServer.h',source/'app/HttpConnectionHandler.cpp'})
    execute(['/usr/bin/clang-format-18','--version'])
    formatter_version=(output/f'command-{len(commands)-1}.stdout').read_text().strip()
    if 'version 18.1.3' not in formatter_version:raise ValueError('formatter version drift')
    execute(['/usr/bin/clang-format-18','-i',*format_files])
    execute(['/usr/bin/clang-format-18','--dry-run','--Werror',*format_files])
    for label,configuration,testing in [('build-B','Debug','ON'),('build-B-release','Release','OFF')]:
        execute(['/usr/bin/cmake','-S',source,'-B',output/label,'-DCMAKE_BUILD_TYPE='+configuration,'-DBUILD_TESTING='+testing,'-DCMAKE_MAKE_PROGRAM=/usr/bin/make'])
        execute(['/usr/bin/cmake','--build',output/label,'-j4'])
    archive.extract_archive((relocated/'inputs/archive/wrk.orig.tar.gz').open('rb'),output/'wrk-source',deadline)
    roots=list((output/'wrk-source').iterdir())
    if len(roots)!=1: raise ValueError('wrk archive root')
    wrk=roots[0];src=wrk/'src'
    archive.extract_archive((relocated/'inputs/archive/wrk.debian.tar.xz').open('rb'),output/'debian',deadline)
    for line in (output/'debian/debian/patches/series').read_text().splitlines():
        if line and not line.startswith('#'):
            if '/' in line or ' ' in line: raise ValueError('patch name')
            execute(['/usr/bin/patch','-p1','--batch','--forward','-i',output/'debian/debian/patches'/line],wrk)
    execute(['/usr/bin/python3','-I',relocated/'baseline/client/patch_wrk.py',src,relocated/'baseline/client'])
    for path in (relocated/'baseline/client').iterdir():
        if path.suffix in ('.c','.h','.inc'):shutil.copy2(path,src/path.name)
    map_patch_before={name:digest(src/name) for name in ('wrk.c','wrk.h')}
    execute(['/usr/bin/python3','-I',relocated/'client/patch_map.py',src,relocated/'client'])
    map_patch_after={name:digest(src/name) for name in ('wrk.c','wrk.h')}
    if any(map_patch_before[name]==map_patch_after[name] for name in map_patch_before):
        raise ValueError('map patch missing')
    if '#include "ClientMap.inc"' not in (src/'wrk.c').read_text():raise ValueError('map include missing')
    for path in (relocated/'client').iterdir():
        if path.suffix in ('.c','.h','.inc'):shutil.copy2(path,src/path.name)
    deb_inputs=[archive.extract_deb(relocated/'inputs/archive/luajit-dev.deb',output/'dev',env,deadline),
                archive.extract_deb(relocated/'inputs/archive/luajit-tool.deb',output/'tool',env,deadline)]
    client=output/'client';client.mkdir()
    execute([output/'tool/usr/bin/luajit','-b',src/'wrk.lua',client/'bytecode.o'])
    (client/'version.c').write_text('const char *VERSION="baseline-v1-map";\n')
    execute(['/usr/bin/cc','-std=c99','-O2','-D_GNU_SOURCE','-D_REENTRANT','-I'+str(src),'-I'+str(wrk),'-I'+str(output/'dev/usr/include/luajit-2.1'),
             *[src/name for name in archive.CLIENT_SOURCES],client/'bytecode.o',client/'version.c',relocated/'runtime/libluajit-5.1.so.2',
             '-Wl,-E','-lpthread','-lm','-ldl','-lssl','-lcrypto','-o',client/'client-map'])
    unit_source=output/'observer-unit-source';unit_source.mkdir()
    shutil.copy2(source/'.clang-format',unit_source/'.clang-format')
    for name in ('RequestBoundaryObserver.h','RequestBoundaryObserver.cpp','RequestBoundaryObserver_test.cpp'):
        shutil.copy2(relocated/'server'/name,unit_source/name)
    unit_files=[unit_source/name for name in ('RequestBoundaryObserver.h','RequestBoundaryObserver.cpp','RequestBoundaryObserver_test.cpp')]
    execute(['/usr/bin/clang-format-18','-i',*unit_files])
    execute(['/usr/bin/clang-format-18','--dry-run','--Werror',*unit_files])
    execute(['/usr/bin/c++','-std=c++20','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(unit_source),
             unit_source/'RequestBoundaryObserver.cpp',unit_source/'RequestBoundaryObserver_test.cpp','-lpthread','-o',output/'observer_sanitized'])
    shutil.copy2(old_binary,output/'server-O')
    libraries={}
    for binary in (output/'server-O',output/'build-B-release/hp_http_server',client/'client-map',output/'observer_sanitized'):
        execute(['/usr/bin/ldd',binary])
        value=(output/f'command-{len(commands)-1}.stdout').read_text()
        if 'not found' in value:raise ValueError('missing runtime library')
        files=[]
        for token in value.split():
            if token.startswith('/'):
                path=Path(token).resolve(strict=True)
                if not any(path.is_relative_to(base) for base in (Path('/usr/lib'),relocated/'runtime')):raise ValueError('undeclared runtime path')
                files.append({'path':str(path),'sha256':digest(path)})
        libraries[str(binary)]={'ldd':value,'files':files,
                                'virtual_kernel_objects':[line.strip() for line in value.splitlines() if 'linux-vdso' in line]}
    receipt={'schema':'request-boundaries-build-v1','relocated_package':str(relocated),'inputs_lock_sha256':digest(relocated/'inputs-lock.json'),
             'server_O':{'path':str(output/'server-O'),'sha256':digest(output/'server-O'),'source_commit':old_build['serverSourceCommit'],'build_receipt_sha256':args.server_o_receipt_sha256},
             'runtime_dependencies':libraries,'commands':commands,'source_commit':old_build['serverSourceCommit'],
             'server_patch':{'before':server_patch_before,'after':server_patch_after,
                             'formatted':{name:digest(source/name) for name in patch_paths}},
             'map_patch':{'before':map_patch_before,'after':map_patch_after},
             'formatter_version':formatter_version,
             'system_tools':system_tools,'system_openssl_headers':openssl_headers,'deb_inputs':deb_inputs,
             'compiled_sources':{str(path.relative_to(output)):digest(path) for path in
                 [source/name for name in patch_paths]+format_files+unit_files+
                 [client/'version.c']+sorted(path for path in src.iterdir() if path.suffix in ('.c','.h','.inc','.lua'))}}
    for key,path in [('server_B',output/'build-B-release/hp_http_server'),('client_map',client/'client-map'),('observer_sanitized',output/'observer_sanitized')]:
        receipt[key]={'path':str(path),'sha256':digest(path)}
    (output/'build-receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')

if __name__=='__main__':main()
