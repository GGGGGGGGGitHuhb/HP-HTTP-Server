"""Offline double-path build. Executed exclusively inside the charged build slot."""
import argparse
import hashlib
import importlib.util
import io
import json
import os
import pathlib
import shutil
import stat
import subprocess
import sys
import tarfile
import time
sys.dont_write_bytecode = True
_dependency = pathlib.Path(__file__).absolute().parent / 'prepare_package.py'
for _path in [*reversed(_dependency.parents), _dependency]:
    _mode = _path.lstat().st_mode
    if stat.S_ISLNK(_mode):
        raise ValueError('linked package import path')
_spec = importlib.util.spec_from_file_location('baseline_prepare_package', _dependency)
_module = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_module)
prepare_package = _module.prepare_package
verify_package = _module.verify_package
validate_directory_path = _module.validate_directory_path

TOOLS = {'cc': '/usr/bin/cc', 'cxx': '/usr/bin/c++', 'cmake': '/usr/bin/cmake',
         'make': '/usr/bin/make', 'python': '/usr/bin/python3', 'patch': '/usr/bin/patch',
         'ldd': '/usr/bin/ldd', 'dpkg_deb': '/usr/bin/dpkg-deb',
         'assembler': '/usr/bin/as', 'linker': '/usr/bin/ld',
         'archiver': '/usr/bin/ar', 'ranlib': '/usr/bin/ranlib'}
CLIENT_SOURCES = ['wrk.c', 'net.c', 'ssl.c', 'aprintf.c', 'stats.c', 'script.c',
                  'units.c', 'ae.c', 'zmalloc.c', 'http_parser.c',
                  'MeasurementWindow.c', 'HistogramExport.c']


def extract_archive(stream, destination, deadline, regular_prefixes=None, ignored_links=None):
    destination.mkdir(parents=True, exist_ok=False)
    skipped_links = []
    with tarfile.open(fileobj=stream, mode='r:*') as archive:
        members = archive.getmembers()
        if len(members) > 20000 or sum(m.size for m in members) > 32 * 1024 * 1024:
            raise ValueError('archive capacity exceeded')
        for member in members:
            if time.monotonic() >= deadline:
                raise TimeoutError('archive verification deadline')
            name = pathlib.PurePosixPath(member.name)
            if name.is_absolute() or '..' in name.parts or not (member.isfile() or member.isdir() or member.issym()):
                raise ValueError('unsafe archive member')
            if member.issym():
                target = pathlib.PurePosixPath(member.linkname)
                if target.is_absolute():
                    raise ValueError('absolute archive link')
                resolved = (destination / name.parent / target).resolve()
                if not resolved.is_relative_to(destination.resolve()):
                    raise ValueError('escaping archive link')
                normalized = name.as_posix()
                if ignored_links is None or ignored_links.get(normalized) != member.linkname:
                    raise ValueError('undeclared archive symlink')
                skipped_links.append({'path':normalized,'target':member.linkname,'reason':'unused Debian alias; not created'})
        # Extract regular files first; never traverse a previously created link.
        for member in members:
            if time.monotonic() >= deadline:
                raise TimeoutError('archive extraction deadline')
            target = destination / member.name
            normalized = pathlib.PurePosixPath(member.name).as_posix()
            if regular_prefixes is not None and not (member.isfile() and
                    any(normalized == prefix or normalized.startswith(prefix + '/') for prefix in regular_prefixes)):
                continue
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.extractfile(member) as source, target.open('xb') as output:
                    shutil.copyfileobj(source, output)
                target.chmod(member.mode & 0o777)
    return skipped_links


def extract_deb(path, destination, environment, deadline):
    # dpkg-deb decodes the declared compression; our extractor owns path safety.
    result = subprocess.run([TOOLS['dpkg_deb'], '--fsys-tarfile', str(path)],
                            env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=max(0.1, deadline - time.monotonic()), check=True)
    if len(result.stdout) > 32 * 1024 * 1024:
        raise ValueError('Debian payload capacity exceeded')
    prefixes = ['usr/include/luajit-2.1'] if path.name == 'luajit-dev.deb' else ['usr/bin/luajit']
    ignored_links = {'usr/lib/x86_64-linux-gnu/libluajit-5.1.so':'libluajit-5.1.so.2.1.1703358377',
                     'usr/share/doc/libluajit-5.1-dev/changelog.Debian.gz':'../libluajit-5.1-2/changelog.Debian.gz'} if path.name == 'luajit-dev.deb' else {
                     'usr/share/doc/luajit/changelog.Debian.gz':'../libluajit-5.1-2/changelog.Debian.gz'}
    skipped = extract_archive(io.BytesIO(result.stdout), destination, deadline,
                              regular_prefixes=prefixes, ignored_links=ignored_links)
    return {'input':str(path),'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),
            'regularPrefixes':prefixes,'skippedLinks':skipped}


def build_one(package, output, deadline):
    output.mkdir(parents=True, exist_ok=False)
    temporary = output / 'tmp'
    temporary.mkdir()
    environment = {'PATH': '/usr/bin:/bin', 'HOME': str(temporary), 'LANG': 'C', 'LC_ALL': 'C',
                   'TMPDIR': str(temporary), 'TMP': str(temporary), 'TEMP': str(temporary),
                   'XDG_CACHE_HOME': str(temporary), 'PYTHONDONTWRITEBYTECODE': '1',
                   'LD_LIBRARY_PATH': str(package / 'runtime'),
                   'LUA_PATH': str(package / 'runtime/lua/?.lua') + ';' + str(package / 'runtime/lua/?/init.lua'),
                   'LUA_CPATH': ''}
    commands = []
    def execute(arguments, cwd=output):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError('build work deadline')
        index = len(commands)
        commands.append([str(value) for value in arguments])
        with (output / f'command-{index}.stdout').open('wb') as stdout, (output / f'command-{index}.stderr').open('wb') as stderr:
            subprocess.run(commands[-1], cwd=cwd, env=environment, stdout=stdout, stderr=stderr,
                           timeout=remaining, check=True)
    for name in TOOLS:
        execute([TOOLS[name], '--version'])
    extract_archive((package / 'inputs/archive/E-S3.tar').open('rb'), output / 'source-E', deadline)
    execute([TOOLS['cmake'], '-S', output / 'source-E', '-B', output / 'build-E',
             '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_TESTING=OFF', '-DCMAKE_CXX_COMPILER=' + TOOLS['cxx'],
             '-DCMAKE_MAKE_PROGRAM=' + TOOLS['make']])
    execute([TOOLS['cmake'], '--build', output / 'build-E', '--target', 'hp_http_server', '-j4'])
    extract_archive((package / 'inputs/archive/wrk.orig.tar.gz').open('rb'), output / 'source-wrk', deadline)
    roots = list((output / 'source-wrk').iterdir())
    if len(roots) != 1 or not roots[0].is_dir():
        raise ValueError('unexpected official wrk archive root')
    wrk = roots[0]
    extract_archive((package / 'inputs/archive/wrk.debian.tar.xz').open('rb'), output / 'debian-input', deadline)
    series = output / 'debian-input/debian/patches/series'
    for line in series.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        if '/' in line or ' ' in line or line in ('.', '..'):
            raise ValueError('unsupported Debian patch series')
        execute([TOOLS['patch'], '-p1', '--batch', '--forward', '-i', series.parent / line], wrk)
    source = wrk / 'src'
    execute([TOOLS['python'], '-I', package / 'client/patch_wrk.py', source, package / 'client'])
    for path in (package / 'client').iterdir():
        if path.suffix in ('.h', '.c', '.inc'):
            shutil.copy2(path, source / path.name)
    deb_receipts = [extract_deb(package / 'inputs/archive/luajit-dev.deb', output / 'luajit-dev', environment, deadline),
                    extract_deb(package / 'inputs/archive/luajit-tool.deb', output / 'luajit-tool', environment, deadline)]
    headers = output / 'luajit-dev/usr/include/luajit-2.1'
    tool = output / 'luajit-tool/usr/bin/luajit'
    client = output / 'client'
    client.mkdir()
    execute([str(tool), '-v'])
    execute([str(tool), '-b', source / 'wrk.lua', client / 'bytecode.o'])
    version = client / 'version.c'
    version.write_text('const char *VERSION="baseline-v1-wrk-4.1.0";\n')
    flags = ['-std=c99', '-O2', '-D_GNU_SOURCE', '-D_REENTRANT', '-I' + str(source),
             '-I' + str(wrk), '-I' + str(headers)]
    execute([TOOLS['cc'], *flags, *[source / name for name in CLIENT_SOURCES], version,
             client / 'bytecode.o', package / 'runtime/libluajit-5.1.so.2.1.1703358377',
             '-Wl,-E', '-lpthread', '-lm', '-ldl', '-lssl', '-lcrypto', '-o', client / 'wrk-baseline'])
    execute([TOOLS['cc'], '-std=c99', '-D_GNU_SOURCE', '-g', '-O1', '-fsanitize=address,undefined',
             '-fno-omit-frame-pointer', '-I' + str(source), '-I' + str(wrk), '-I' + str(headers),
             package / 'tests/MeasurementWindow_test.c',
             *[source / name for name in CLIENT_SOURCES if name != 'wrk.c'], version,
             client / 'bytecode.o', package / 'runtime/libluajit-5.1.so.2.1.1703358377',
             '-Wl,-E', '-lm', '-lpthread', '-ldl', '-lssl', '-lcrypto', '-o', client / 'unit-sanitized'])
    library_receipts = {}
    for binary in (client / 'wrk-baseline', client / 'unit-sanitized', output / 'build-E/hp_http_server', tool):
        execute([TOOLS['ldd'], binary])
        text = (output / f'command-{len(commands)-1}.stdout').read_text()
        loaded = []
        for line in text.splitlines():
            for token in line.split():
                if not token.startswith('/'):
                    continue
                library = pathlib.Path(token).resolve(strict=True)
                if not any(library.is_relative_to(base) for base in
                           (pathlib.Path('/usr/lib'), pathlib.Path('/lib'), package / 'runtime')):
                    raise ValueError('undeclared loaded library path')
                loaded.append({'path': str(library), 'sha256': hashlib.sha256(library.read_bytes()).hexdigest()})
        if 'not found' in text:
            raise ValueError('missing runtime dependency')
        library_receipts[str(binary)] = {'ldd': text, 'files': loaded,
                                        'kernelVirtualObjects': ['linux-vdso.so.1'] if 'linux-vdso.so.1' in text else []}
    loader_lines = library_receipts[str(client / 'wrk-baseline')]['ldd']
    if 'not found' in loader_lines or str(package / 'runtime/libluajit-5.1.so.2') not in loader_lines:
        raise ValueError('runtime dependency identity mismatch')
    return {'packageRoot': str(package), 'outputRoot': str(output), 'commands': commands,
            'serverSha256': hashlib.sha256((output / 'build-E/hp_http_server').read_bytes()).hexdigest(),
            'clientSha256': hashlib.sha256((client / 'wrk-baseline').read_bytes()).hexdigest(),
            'unitSha256': hashlib.sha256((client / 'unit-sanitized').read_bytes()).hexdigest(),
            'systemTools': {name: {'path': path, 'sha256': hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()}
                            for name, path in TOOLS.items()},
            'actualLoadedLibraries': library_receipts,
            'debianExtraction': deb_receipts,
            'serverSourceCommit': 'acda3f92d42a36d0b0554e185bc6f4155b4e5889',
            'serverSourceFiles': [{'path': path.relative_to(output / 'source-E').as_posix(),
                                   'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
                                  for path in sorted((output / 'source-E').rglob('*')) if path.is_file()],
            'opensslHeaders': [{'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
                               for base in (pathlib.Path('/usr/include/openssl'),
                                            pathlib.Path('/usr/include/x86_64-linux-gnu/openssl'))
                               for path in sorted(base.rglob('*.h'))]}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--package-root', required=True)
    parser.add_argument('--output-root', required=True)
    args = parser.parse_args()
    if 'HP_BASELINE_WORK_DEADLINE' not in os.environ or 'HP_BASELINE_CLEANUP_DEADLINE' not in os.environ:
        raise ValueError('missing explicit outer build reservation deadlines')
    work_deadline = float(os.environ['HP_BASELINE_WORK_DEADLINE'])
    cleanup_deadline = float(os.environ['HP_BASELINE_CLEANUP_DEADLINE'])
    if not time.monotonic() < work_deadline < cleanup_deadline:
        raise ValueError('invalid outer build reservation deadlines')
    deadline = min(time.monotonic() + 160, work_deadline)
    package = validate_directory_path(args.package_root)
    if package != pathlib.Path(__file__).absolute().parent:
        raise ValueError('build entry package identity mismatch')
    output = pathlib.Path(args.output_root).absolute()
    validate_directory_path(output.parent)
    _module.verify_execution_context(package,output.parent,'run-r015-build-001')
    verify_package(package)
    required_inputs = ['inputs/archive/E-S3.tar','inputs/archive/wrk.orig.tar.gz',
                       'inputs/archive/wrk.debian.tar.xz','inputs/archive/luajit-dev.deb',
                       'inputs/archive/luajit-tool.deb','client/patch_wrk.py',
                       'client/MeasurementWindow.c','client/MeasurementWindow.h',
                       'client/HistogramExport.c','client/HistogramExport.h',
                       'client/BaselineMain.inc','client/BaselineCallbacks.inc',
                       'client/BaselineRuntime.inc','client/BaselineExport.inc',
                       'runtime/libluajit-5.1.so.2','tests/MeasurementWindow_test.c',
                       'config/request.bin','config/payload-1024.bin','config/workload.json']
    for relative in required_inputs:
        if not (package/relative).is_file():
            raise ValueError('missing required build input: '+relative)
    for name, path in TOOLS.items():
        if not pathlib.Path(path).is_file():
            raise ValueError('missing explicit system tool: ' + name)
    if not pathlib.Path('/usr/include/openssl/ssl.h').is_file():
        raise ValueError('missing explicit system OpenSSL development headers')
    output.mkdir(parents=True, exist_ok=False)
    relocated = prepare_package(package, output / 'relocated-package')
    receipts = [build_one(package, output / 'native', deadline),
                build_one(relocated, output / 'relocated', deadline)]
    (output / 'build-receipt.json').write_text(json.dumps({'schema': 'baseline-v1-build', 'builds': receipts}, indent=2) + '\n')

if __name__ == '__main__':
    main()
