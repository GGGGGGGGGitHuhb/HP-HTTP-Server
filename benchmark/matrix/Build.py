#!/usr/bin/env python3
"""Export the fixed S1 commit and build one independent Release product."""
import argparse
import hashlib
import io
import json
import pathlib
import re
import subprocess
import tarfile

from Common import COMMIT, TREE, REPO, FLAGS, WRK, WRK_SHA, LIB, Budget, OwnedProcess
from Common import demand, fresh_directory, role_root, save, sha, wait_process


def archive_bytes():
    actual = subprocess.check_output(['git', 'rev-parse', COMMIT + '^{tree}'], cwd=REPO, text=True).strip()
    demand(actual == TREE, 'fixed commit/tree mismatch')
    return subprocess.check_output(['git', 'archive', '--format=tar', COMMIT], cwd=REPO)


def libraries(binary):
    result = subprocess.run(['ldd', str(binary)], capture_output=True, text=True, check=True)
    demand('not found' not in result.stdout, 'runtime library missing')
    return {match.group(1): sha(match.group(1)) for match in re.finditer(r'(?:=>\s+)?(/\S+)\s+\(', result.stdout)}


def validate_tool():
    demand(WRK.is_file() and sha(WRK) == WRK_SHA, 'fixed wrk missing or changed')
    result = subprocess.run([str(WRK), '--version'], capture_output=True, text=True, timeout=5)
    demand('wrk debian/4.1.0-4build2' in result.stdout + result.stderr, 'wrk cannot run fixed version')
    return {'binary': str(WRK), 'sha256': WRK_SHA, 'version': (result.stdout + result.stderr).splitlines()[0], 'libraries': libraries(WRK)}


def validate_manifest(path):
    data = json.loads(pathlib.Path(path).read_text())
    demand(data['commit'] == COMMIT and data['tree'] == TREE and data['flags'] == FLAGS, 'manifest fixed identity/flags mismatch')
    archive = archive_bytes()
    demand(data['archive_sha256'] == hashlib.sha256(archive).hexdigest() == sha(data['archive']), 'archive drift')
    with tarfile.open(fileobj=io.BytesIO(archive)) as stream:
        expected = {member.name: hashlib.sha256(stream.extractfile(member).read()).hexdigest() for member in stream if member.isfile()}
    demand(data['source_hashes'] == expected, 'source manifest drift')
    source = pathlib.Path(data['source'])
    actual = {str(p.relative_to(source)): sha(p) for p in source.rglob('*') if p.is_file()}
    demand(actual == expected, 'source drift including extra files')
    for name in ('binary', 'cmake_cache', 'compile_commands'):
        demand(sha(data[name]) == data[name + '_sha256'], name + ' drift')
    for item in json.loads(pathlib.Path(data['compile_commands']).read_text()):
        command = item['command']
        demand(all(flag in command for flag in ('-O3', '-DNDEBUG', '-std=c++20')), 'compiled Release flags missing')
        demand(not any(flag in command for flag in ('-fsanitize', '-flto', '-march', '-mtune')), 'forbidden compile flags')
    demand(libraries(data['binary']) == data['server_libraries'], 'server libraries changed')
    demand(validate_tool() == data['wrk'], 'wrk/tool libraries changed')
    demand(sha('/usr/bin/g++') == data['compiler_sha256'], 'compiler changed')
    return data


def build(root):
    budget = Budget(root, create=True)
    budget.begin('build')
    child = None
    try:
        output = fresh_directory(root / 'artifact')
        archive = archive_bytes()
        (output / 'source.tar').write_bytes(archive)
        source = fresh_directory(output / 'source')
        with tarfile.open(fileobj=io.BytesIO(archive)) as stream:
            stream.extractall(source, filter='data')
        tool = validate_tool()
        commands = [['cmake', '-S', str(source), '-B', str(output / 'build'), *FLAGS],
                    ['cmake', '--build', str(output / 'build'), '--target', 'hp_http_server', '-j2']]
        for name, command in zip(('configure', 'build'), commands):
            child = OwnedProcess(command, output / name)
            demand(wait_process(child, budget, budget.phase_deadline) == 0, name + ' failed')
            child.close(budget.data['deadline_monotonic'])
            child = None
        data = {'schema': 1, 'commit': COMMIT, 'tree': TREE, 'archive': str(output / 'source.tar'),
                'archive_sha256': hashlib.sha256(archive).hexdigest(), 'source': str(source),
                'source_hashes': {str(p.relative_to(source)): sha(p) for p in source.rglob('*') if p.is_file()},
                'flags': FLAGS, 'commands': commands, 'compiler': subprocess.check_output(['/usr/bin/g++', '--version'], text=True).splitlines()[0],
                'compiler_sha256': sha('/usr/bin/g++'), 'cmake': subprocess.check_output(['cmake', '--version'], text=True).splitlines()[0], 'wrk': tool}
        for name, path in [('binary', output / 'build/hp_http_server'), ('cmake_cache', output / 'build/CMakeCache.txt'), ('compile_commands', output / 'build/compile_commands.json')]:
            data[name], data[name + '_sha256'] = str(path), sha(path)
        data['server_libraries'] = libraries(data['binary'])
        save(output / 'manifest.json', data)
        validate_manifest(output / 'manifest.json')
        budget.finish('passed')
        print(output / 'manifest.json')
    except BaseException as error:
        if child:
            child.close(budget.data['deadline_monotonic'])
        save(root / 'build-failure.json', {'status': 'invalid', 'error': repr(error)})
        budget.finish('invalid', repr(error))
        raise


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True)
    args = parser.parse_args()
    build(role_root(args.root))
