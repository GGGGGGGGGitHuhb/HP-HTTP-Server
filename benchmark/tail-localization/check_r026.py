"""R022 external charged check: real build receipt, original CTest, sanitizer and decoder seams."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import time
import unittest
sys.dont_write_bytecode=True
ROOT=Path(__file__).absolute().parent


def regular(path):
    path=Path(path).absolute()
    for ancestor in reversed(path.parents):
        if not stat.S_ISDIR(ancestor.lstat().st_mode):raise ValueError('linked/non-directory input parent')
    if not stat.S_ISREG(path.lstat().st_mode):raise ValueError('linked/non-regular input')
    return path


def digest(path):
    return hashlib.sha256(regular(path).read_bytes()).hexdigest()


def load_own(path,name):
    spec=importlib.util.spec_from_file_location(name,regular(path))
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    return module


def main():
    parser=argparse.ArgumentParser()
    for flag in ('package-root','build-output','output-root','governance-test','governance-test-sha256','governance-module-sha256','driver-manifest','driver-manifest-sha256'):
        parser.add_argument('--'+flag,required=True)
    parser.add_argument('--verification-bundle')
    parser.add_argument('--verification-bundle-sha256')
    parser.add_argument('--contract-bundle');parser.add_argument('--contract-bundle-sha256')
    args=parser.parse_args()
    manifest_path=regular(args.driver_manifest)
    if digest(manifest_path)!=args.driver_manifest_sha256:raise ValueError('R022 driver manifest drift')
    driver_manifest=json.loads(manifest_path.read_text())
    for item in driver_manifest['files']:
        path=regular(item['path'])
        if path.stat().st_size!=item['bytes'] or digest(path)!=item['sha256']:raise ValueError('R022 driver closure drift')
        if path.suffix=='.py':compile(path.read_bytes(),str(path),'exec')
    primary=regular(Path(args.package_root)/'inputs-lock.json').parent
    lock_path=regular(primary/'inputs-lock.json')
    if digest(lock_path)!=os.environ.get('HP_BASELINE_PACKAGE_SHA256'):raise ValueError('preimport package binding')
    lock=json.loads(lock_path.read_text());declared={row['path']:row for row in lock['files']}
    if digest(primary/'prepare_package.py')!=declared['prepare_package.py']['sha256']:raise ValueError('preimport prepare binding')
    prepare=load_own(primary/'prepare_package.py','r022_check_prepare')
    primary=prepare.validate_directory_path(args.package_root)
    prepare.verify_package(primary)
    output=prepare.validate_directory_path(args.output_root)
    prepare.verify_execution_context(primary,output,'run-r022-check-001')
    deadline=float(os.environ['HP_BASELINE_WORK_DEADLINE'])
    if not time.monotonic()<deadline<float(os.environ['HP_BASELINE_CLEANUP_DEADLINE']):raise ValueError('outer deadline')
    build=prepare.validate_directory_path(args.build_output)
    receipt_path=regular(build/'build-receipt.json')
    if digest(receipt_path)!=os.environ['HP_BASELINE_BUILD_RECEIPT_SHA256']:raise ValueError('build receipt binding')
    receipt=json.loads(receipt_path.read_text())
    if receipt['schema']!='request-boundaries-build-v1' or receipt['source_commit']!=lock['serverCommit']:
        raise ValueError('actual build receipt/source schema')
    package=prepare.validate_directory_path(receipt['relocated_package'])
    if package!=build/'relocated-package':raise ValueError('build relocated package route')
    prepare.verify_package(package)
    if digest(package/'inputs-lock.json')!=digest(lock_path) or receipt['inputs_lock_sha256']!=digest(lock_path):
        raise ValueError('primary/relocated lock mismatch')
    for name in ('server_O','server_B','client_map','observer_sanitized'):
        entry=receipt[name];path=regular(entry['path'])
        if not path.is_relative_to(build) or digest(path)!=entry['sha256']:raise ValueError('build binary drift')
        for library in receipt['runtime_dependencies'][str(path)]['files']:
            if digest(library['path'])!=library['sha256']:raise ValueError('build runtime drift')
    patch=receipt['server_patch']
    if set(patch['before'])!=set(patch['after']) or set(patch['before'])!=set(patch['formatted']):raise ValueError('patch stages differ')
    for name in patch['before']:
        relative=Path(name)
        if relative.is_absolute() or '..' in relative.parts:raise ValueError('patch receipt path')
        if patch['before'][name]==patch['after'][name] or digest(build/'source-B'/relative)!=patch['formatted'][name]:
            raise ValueError('actual patch stage identity')
    for name,sha in receipt['compiled_sources'].items():
        relative=Path(name)
        if relative.is_absolute() or '..' in relative.parts or digest(build/relative)!=sha:raise ValueError('compiled source drift')
    if 'version 18.1.3' not in receipt['formatter_version']:raise ValueError('formatter receipt')
    if set(receipt['map_patch']['before'])!={'wrk.c','wrk.h'} or set(receipt['map_patch']['after'])!={'wrk.c','wrk.h'}:
        raise ValueError('map patch exact source set')
    wrk_roots=list((build/'wrk-source').iterdir())
    if len(wrk_roots)!=1:raise ValueError('actual wrk source root')
    for name in receipt['map_patch']['before']:
        if receipt['map_patch']['before'][name]==receipt['map_patch']['after'][name] or digest(wrk_roots[0]/'src'/name)!=receipt['map_patch']['after'][name]:
            raise ValueError('actual map patch identity')
    for tool in receipt['system_tools'].values():
        if digest(tool['realpath'])!=tool['sha256']:raise ValueError('system tool drift')
    for name,sha in receipt['system_openssl_headers'].items():
        if digest(name)!=sha:raise ValueError('OpenSSL development header drift')
    governance=regular(args.governance_test);governance_module=regular(governance.with_name('r018_admission.py'))
    if digest(governance)!=args.governance_test_sha256 or digest(governance_module)!=args.governance_module_sha256:
        raise ValueError('governance producer closure drift')
    compiled=[]
    for path in sorted(package.rglob('*.py')):
        compile(regular(path).read_bytes(),str(path),'exec');compiled.append(path.relative_to(package).as_posix())
    for path in (governance,governance_module):compile(path.read_bytes(),str(path),'exec')
    reviewer_entry=None
    if os.environ['HP_BASELINE_ROLE']=='reviewer':
        if not args.verification_bundle or not args.verification_bundle_sha256:raise ValueError('Reviewer verification binding missing')
        bundle=prepare.validate_directory_path(args.verification_bundle)
        actual=set()
        for path in bundle.rglob('*'):
            mode=path.lstat().st_mode
            if not (stat.S_ISDIR(mode) or stat.S_ISREG(mode)):raise ValueError('verification links/special files')
            if stat.S_ISREG(mode):actual.add(path.relative_to(bundle).as_posix())
        if digest(bundle/'inputs.json')!=args.verification_bundle_sha256:raise ValueError('verification manifest drift')
        manifest=json.loads((bundle/'inputs.json').read_text())
        if manifest['schema']!='request-boundaries-reviewer-verification-v1' or manifest['role']!='reviewer':raise ValueError('verification manifest schema/role')
        entries={row['path']:row for row in manifest['files']}
        if len(entries)!=len(manifest['files']) or set(entries)|{'inputs.json'}!=actual or not 1<=len(entries)<=16:
            raise ValueError('verification exact bounded closure')
        for name,row in entries.items():
            relative=Path(name)
            if relative.is_absolute() or '..' in relative.parts or relative.suffix not in ('.py','.md'):raise ValueError('verification source path')
            path=regular(bundle/relative)
            if path.stat().st_size!=row['bytes'] or digest(path)!=row['sha256']:raise ValueError('verification source drift')
            if relative.suffix=='.py':compile(path.read_bytes(),str(path),'exec')
        if manifest['entry'] not in entries or Path(manifest['entry']).suffix!='.py':raise ValueError('verification entry outside declared Python closure')
        reviewer_entry=bundle/manifest['entry']
    elif args.verification_bundle or args.verification_bundle_sha256:raise ValueError('Reviewer verification supplied to Builder')
    scratch=output/'fixtures';scratch.mkdir(exist_ok=False)
    env={name:value for name,value in os.environ.items() if name.startswith(('HP_BASELINE_','HP_R022_'))}
    env['HP_R022_PRODUCTION_ROOT']=driver_manifest['production_root']
    env.update(PATH='/usr/bin:/bin',HOME=str(scratch),TMPDIR=str(scratch),TMP=str(scratch),TEMP=str(scratch),
        XDG_CACHE_HOME=str(scratch),LANG='C',LC_ALL='C',LD_LIBRARY_PATH=str(package/'runtime'),
        PYTHONDONTWRITEBYTECODE='1',
        LUA_PATH=str(package/'runtime/lua/?.lua')+';'+str(package/'runtime/lua/?/init.lua'),LUA_CPATH='',
        ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1',
        HP_S3_TEST_TMP_ROOT=str(build/'build-B/test-tmp'))
    runs=[]
    status={'schema':'request-boundaries-check-v1','status':'running','build_receipt_sha256':digest(receipt_path),'runs':runs}
    def execute(label,argv,cwd=package):
        remaining=deadline-time.monotonic()
        if remaining<=0:raise TimeoutError('check absolute work deadline')
        command=[str(value) for value in argv]
        row={'name':label,'argv':command,'cwd':str(cwd)};runs.append(row)
        with (output/(label+'.stdout')).open('xb') as stdout,(output/(label+'.stderr')).open('xb') as stderr:
            response=subprocess.run(command,cwd=cwd,env=env,stdout=stdout,stderr=stderr,timeout=remaining)
        row['returncode']=response.returncode
        if response.returncode!=0:raise RuntimeError(label+' failed')
        diagnostics=(output/(label+'.stderr')).read_text(errors='replace')
        if label=='observer-unit' and any(marker in diagnostics for marker in ('ERROR: AddressSanitizer','runtime error:','UndefinedBehaviorSanitizer','LeakSanitizer')):
            raise RuntimeError('actual sanitizer diagnostics')
    try:
        execute('governance',['/usr/bin/python3','-I','-B',governance],output)
        if reviewer_entry:execute('reviewer-verification',['/usr/bin/python3','-I','-B',reviewer_entry],output)
        execute('ctest-list',['/usr/bin/ctest','--test-dir',build/'build-B','--show-only=json-v1'])
        catalog=json.loads((output/'ctest-list.stdout').read_text())
        expected={'cli_tests','http_server_integration_tests','http_keep_alive_integration_tests','server_integration_tests',
                  'benchmark_runner_tests','async_logger_batch_tests','r6_callbacks_tests','tcp_nodelay_tests','tcp_nodelay_http_tests'}
        if len(catalog['tests'])!=9 or {row['name'] for row in catalog['tests']}!=expected:raise ValueError('actual original nine CTest catalog')
        overlay=load_own(ROOT/'ctest_overlay_r022.py','r022_overlay')
        overlay_dir,benchmark_temp=overlay.create(build,output,catalog)
        execute('ctest-overlay-list',['/usr/bin/ctest','--test-dir',overlay_dir,'--show-only=json-v1'])
        actual=json.loads((output/'ctest-overlay-list.stdout').read_text())
        status['catalog_contract']=overlay.compare(catalog,actual,build,benchmark_temp)
        env['HP_R022_ORIGINAL_CATALOG']=str(output/'ctest-list.stdout')
        env['HP_R022_OVERLAY_CATALOG']=str(output/'ctest-overlay-list.stdout')
        execute('contract',['/usr/bin/python3','-I','-B',ROOT/'test_r026.py'],output)
        if os.environ['HP_BASELINE_ROLE']=='reviewer':
            if not args.contract_bundle or not args.contract_bundle_sha256:raise ValueError('Reviewer contract bundle required')
            bundle=regular(Path(args.contract_bundle)/'inputs.json')
            if digest(bundle)!=args.contract_bundle_sha256:raise ValueError('Reviewer contract bundle drift')
            content=json.loads(bundle.read_text());entries={row['path']:row for row in content['files']}
            if len(entries)!=len(content['files']) or content['role']!='reviewer' or content['entry'] not in entries or not content['entry'].endswith('.py'):raise ValueError('Reviewer contract entry')
            observed={str(path.relative_to(bundle.parent)) for path in bundle.parent.rglob('*') if not path.is_dir()}
            if observed!=set(entries)|{'inputs.json'}:raise ValueError('Reviewer contract exact files')
            for name,row in entries.items():
                path=Path(name)
                if path.is_absolute() or '..' in path.parts:raise ValueError('Reviewer contract relative path')
                source=regular(bundle.parent/path)
                if source.stat().st_size!=row['bytes'] or digest(source)!=row['sha256']:raise ValueError('Reviewer contract closure drift')
                if source.suffix=='.py':compile(source.read_bytes(),str(source),'exec')
            execute('reviewer-contract',['/usr/bin/python3','-I','-B',bundle.parent/content['entry']],output)
        elif args.contract_bundle or args.contract_bundle_sha256:raise ValueError('Builder unexpected Reviewer contract bundle')
        execute('ctest',['/usr/bin/ctest','--test-dir',overlay_dir,'--output-on-failure','-j2'])
        execute('observer-unit',[receipt['observer_sanitized']['path']])
        exports=list(scratch.glob('hp-r018-unit-*/boundary-worker-0.bin'))
        if len(exports)!=1:raise ValueError('actual unit export missing/ambiguous')
        wire=regular(exports[0]).read_bytes()
        if len(wire)!=256:raise ValueError('actual serializer size')
        header=struct.unpack('<8sII10Q',wire[:96])
        if header[:3]!=(b'HPBOUND1',1,0) or header[6:]!=(2,1,1,0,1,1,0):raise ValueError('actual serializer header/count/status')
        if wire[160:168]!=b'HPBTAIL1' or wire[168:]!=wire[8:96]:raise ValueError('actual serializer trailer')
        if struct.unpack('<IIQQQ',wire[96:128])!=(1,3,1,100,200) or struct.unpack('<IIQQQ',wire[128:160])!=(1,1,2,300,0):
            raise ValueError('actual serializer 32LE record')
        tests=load_own(package/'tests/test_boundaries.py','r018_check_tests')
        with (output/'decoder-tests.txt').open('x') as stream:
            result=unittest.TextTestRunner(stream=stream,verbosity=2).run(tests.create_suite(package,scratch))
        status['decoder_tests']=result.testsRun
        if not result.wasSuccessful() or result.testsRun!=6:raise RuntimeError('actual decoder seam fixtures failed')
        execute('integration',['/usr/bin/python3','-I','-B',package/'integration_boundaries.py','--package-root',package,
                               '--build-output',build,'--output-root',output/'integration'])
        status.update(status='valid',python_compiled=compiled,ctest_count=9,unit_export_sha256=digest(exports[0]),
                      governance_test_sha256=args.governance_test_sha256,governance_module_sha256=args.governance_module_sha256)
    except BaseException as error:
        status.update(status='failed',first_failure={'type':type(error).__name__,'message':str(error)})
        raise
    finally:
        (output/'check-receipt.json').write_text(json.dumps(status,indent=2,allow_nan=False)+'\n')

if __name__=='__main__':
    try:
        main()
    except BaseException as first_error:
        # Preserve preflight failure only in the already granted exact check output.
        try:
            failure_root=Path(os.environ['HP_BASELINE_OUTPUT_ROOT']).absolute()
            role=os.environ.get('HP_BASELINE_ROLE')
            if role not in ('builder','reviewer') or failure_root.parent.name!=role or os.environ.get('HP_BASELINE_RUN')!='run-r022-check-001' or failure_root.name!='run-r022-check-001':
                raise ValueError('not an exact check grant')
            for parent in (*reversed(failure_root.parents),failure_root):
                if not stat.S_ISDIR(parent.lstat().st_mode):raise ValueError('unsafe failure output')
            with (failure_root/'check-receipt.json').open('x') as stream:
                json.dump({'schema':'request-boundaries-check-v1','status':'failed',
                           'first_failure':{'type':type(first_error).__name__,'message':str(first_error)}},stream)
                stream.write('\n')
        except (OSError,ValueError,KeyError):
            pass
        raise
