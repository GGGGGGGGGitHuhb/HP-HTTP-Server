"""Charged pure validation of the exact relocated package and built client seams."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import time
import unittest
sys.dont_write_bytecode = True
PACKAGE = Path(__file__).absolute().parent
for path in [*reversed(PACKAGE.parents), PACKAGE]:
    if stat.S_ISLNK(path.lstat().st_mode):
        raise ValueError('linked package root')


def load_own(path, name):
    if stat.S_ISLNK(path.lstat().st_mode):
        raise ValueError('linked package module')
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--package-root', required=True)
    parser.add_argument('--build-output', required=True)
    parser.add_argument('--output-root', required=True)
    parser.add_argument('--governance-test', required=True)
    parser.add_argument('--governance-test-sha256', required=True)
    parser.add_argument('--governance-module-sha256', required=True)
    parser.add_argument('--verification-bundle',dest='reviewer_bundle')
    parser.add_argument('--verification-bundle-sha256',dest='reviewer_bundle_sha256')
    args = parser.parse_args()
    prepare = load_own(PACKAGE / 'prepare_package.py', 'baseline_check_prepare')
    root = prepare.validate_directory_path(args.package_root)
    if root != PACKAGE:
        raise ValueError('entry package identity mismatch')
    prepare.verify_package(root)
    output = prepare.validate_directory_path(args.output_root)
    prepare.verify_execution_context(root,output,'run-r015-check-001')
    build = prepare.validate_directory_path(args.build_output)
    deadline = float(os.environ['HP_BASELINE_WORK_DEADLINE'])
    if not time.monotonic() < deadline < float(os.environ['HP_BASELINE_CLEANUP_DEADLINE']):
        raise ValueError('invalid outer pure check reservation')
    receipt_path = build / 'build-receipt.json'
    if receipt_path.is_symlink() or not receipt_path.is_file():
        raise ValueError('linked or missing build receipt')
    if hashlib.sha256(receipt_path.read_bytes()).hexdigest() != os.environ['HP_BASELINE_BUILD_RECEIPT_SHA256']:
        raise ValueError('build receipt drift')
    receipt = json.loads(receipt_path.read_text())
    reviewer_test = None
    if os.environ['HP_BASELINE_ROLE']=='reviewer':
        if not args.reviewer_bundle or not args.reviewer_bundle_sha256:
            raise ValueError('missing independent Reviewer bundle binding')
        bundle = prepare.validate_directory_path(args.reviewer_bundle)
        for path in bundle.rglob('*'):
            if path.is_symlink() or not (path.is_dir() or path.is_file()):
                raise ValueError('linked or special Reviewer bundle member')
        bundle_manifest = bundle/'inputs.json'
        manifest_bytes = bundle_manifest.read_bytes()
        if hashlib.sha256(manifest_bytes).hexdigest()!=args.reviewer_bundle_sha256:
            raise ValueError('Reviewer bundle input identity drift')
        manifest = json.loads(manifest_bytes)
        if manifest.get('schema')!='r015-reviewer-verification-bundle-v1' or manifest.get('role')!='reviewer' or \
           len(manifest['files'])!=3 or {entry['path'] for entry in manifest['files']}!={
                'r015_reference_001.py','r015_reference_test_001.py','r015_review_verify_001.py'}:
            raise ValueError('Reviewer bundle closure contract drift')
        declared = {'inputs.json'}
        for entry in manifest['files']:
            relative = Path(entry['path'])
            if relative.is_absolute() or '..' in relative.parts or entry['path'] in declared:
                raise ValueError('invalid Reviewer source path')
            path = bundle/relative
            data = path.read_bytes()
            if len(data)!=entry['bytes'] or hashlib.sha256(data).hexdigest()!=entry['sha256']:
                raise ValueError('Reviewer source identity drift')
            if path.suffix!='.py':
                raise ValueError('unexpected Reviewer source type')
            compile(data,str(path),'exec')
            declared.add(entry['path'])
        if {path.relative_to(bundle).as_posix() for path in bundle.rglob('*') if path.is_file()}!=declared:
            raise ValueError('Reviewer bundle file set drift')
        reviewer_test = bundle/'r015_reference_test_001.py'
    elif args.reviewer_bundle or args.reviewer_bundle_sha256:
        raise ValueError('Reviewer bundle supplied to Builder scope')
    compiled = []
    for path in sorted(root.rglob('*.py')):
        compile(path.read_bytes(), str(path), 'exec')
        compiled.append(path.relative_to(root).as_posix())
    scratch = output / 'fixtures'
    scratch.mkdir(exist_ok=False)
    tests = load_own(root / 'tests/test_package.py', 'baseline_check_tests')
    with (output / 'python-tests.txt').open('x') as stream:
        result = unittest.TextTestRunner(stream=stream, verbosity=2).run(tests.create_suite(root, scratch))
    if not result.wasSuccessful():
        raise RuntimeError('actual Python pure tests failed')
    runs = []
    for position, row in enumerate(receipt['builds']):
        package = prepare.validate_directory_path(row['packageRoot'])
        prepare.verify_package(package)
        binary_root = prepare.validate_directory_path(row['outputRoot'])
        prepare.validate_directory_path(binary_root/'client')
        client = binary_root / 'client/wrk-baseline'
        if client.is_symlink() or (binary_root/'client/unit-sanitized').is_symlink():
            raise ValueError('linked built client input')
        if hashlib.sha256(client.read_bytes()).hexdigest() != row['clientSha256']:
            raise ValueError('built client drift')
        if hashlib.sha256((binary_root/'client/unit-sanitized').read_bytes()).hexdigest() != row['unitSha256']:
            raise ValueError('built sanitizer unit drift')
        environment = {'PATH':'/usr/bin:/bin', 'HOME':str(scratch), 'LANG':'C', 'LC_ALL':'C',
                       'TMPDIR':str(scratch), 'TMP':str(scratch), 'TEMP':str(scratch),
                       'XDG_CACHE_HOME':str(scratch), 'LD_LIBRARY_PATH':str(package/'runtime'),
                       'LUA_PATH':str(package/'runtime/lua/?.lua')+';'+str(package/'runtime/lua/?/init.lua'),
                       'LUA_CPATH':'', 'HP_BASELINE_WARMUP_SECONDS':'1',
                       'HP_BASELINE_OUTPUT_DIR':str(scratch),
                       'ASAN_OPTIONS':'detect_leaks=1:halt_on_error=1', 'UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'}
        commands = [[str(binary_root/'client/unit-sanitized')],
                    [str(client), '-t2','-c128','-d3s','--timeout','2s','-s',
                     str(scratch/'old-workspace-denied.lua'), 'http://127.0.0.1:1/payload-1024.bin']]
        for index, command in enumerate(commands):
            with (output/f'client-{position}-{index}.stdout').open('xb') as stdout, \
                 (output/f'client-{position}-{index}.stderr').open('xb') as stderr:
                response = subprocess.run(command, cwd=package, env=environment, stdout=stdout, stderr=stderr,
                                          timeout=max(0.1, deadline-time.monotonic()))
            expected = 0 if index == 0 else 2
            if response.returncode != expected:
                raise RuntimeError('actual C unit or forbidden config test failed')
            runs.append({'argv':command,'returncode':response.returncode,'packageRoot':str(package)})
    governance = Path(args.governance_test).absolute()
    prepare.validate_directory_path(governance.parent)
    if governance.is_symlink() or hashlib.sha256(governance.read_bytes()).hexdigest() != args.governance_test_sha256:
        raise ValueError('explicit governance test identity mismatch')
    governance_module = governance.with_name('r015_admission.py')
    if governance_module.is_symlink() or hashlib.sha256(governance_module.read_bytes()).hexdigest() != args.governance_module_sha256:
        raise ValueError('explicit governance module identity mismatch')
    with (output/'governance-tests.stdout').open('xb') as stdout, (output/'governance-tests.stderr').open('xb') as stderr:
        governance_environment = {name:os.environ[name] for name in os.environ if name.startswith('HP_BASELINE_')}
        governance_environment.update(PATH='/usr/bin:/bin',HOME=str(scratch),LANG='C',LC_ALL='C',
                                      TMPDIR=str(scratch),TMP=str(scratch),TEMP=str(scratch),
                                      XDG_CACHE_HOME=str(scratch),PYTHONDONTWRITEBYTECODE='1')
        response = subprocess.run(['/usr/bin/python3','-I',str(governance)], cwd=output, env=governance_environment,
                                  stdout=stdout, stderr=stderr, timeout=max(0.1, deadline-time.monotonic()), check=True)
    if reviewer_test:
        with (output/'reviewer-reference.stdout').open('xb') as stdout, (output/'reviewer-reference.stderr').open('xb') as stderr:
            subprocess.run(['/usr/bin/python3','-I','-B',str(reviewer_test)], cwd=output,env=governance_environment,
                           stdout=stdout,stderr=stderr,timeout=max(0.1,deadline-time.monotonic()),check=True)
    (output/'check-receipt.json').write_text(json.dumps({'schema':'baseline-v1-check','status':'valid',
        'pythonCompiled':compiled,'pythonTests':result.testsRun,'clientChecks':runs,
        'governanceTest':str(governance),'governanceSha256':args.governance_test_sha256,
        'governanceModule':str(governance_module),'governanceModuleSha256':args.governance_module_sha256,
        'reviewerBundle':args.reviewer_bundle,'reviewerBundleSha256':args.reviewer_bundle_sha256},indent=2)+'\n')

if __name__ == '__main__':
    main()
