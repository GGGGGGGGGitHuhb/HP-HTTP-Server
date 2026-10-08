"""R022 check recovery and one declared sparse synthetic-file accounting rule."""
import hashlib
import json
import math
import os
from pathlib import Path
import stat
import r021_admission as previous
from r019_capacity import measure_declared_root
original=previous.original
require=original.require
CURRENT_CLAIM=None
CONTRACT_CONTEXT=None
SPARSE_EVIDENCE=[]
NEW_CHECK='run-r022-check-001'

def record_build_artifact(args,reservation,environment):
    if args.run_id!=NEW_CHECK:return original.record_build_artifact(args,reservation,environment)
    reservation.record['r018_package_sha256']=environment['HP_BASELINE_PACKAGE_SHA256']
    reservation.record['r018_build_receipt_sha256']=environment['HP_BASELINE_BUILD_RECEIPT_SHA256']

def set_claim(value):
    global CURRENT_CLAIM
    CURRENT_CLAIM=value
    previous.set_claim(value)

def get_claim():return CURRENT_CLAIM

def set_contract_claim(called_path,called_sha256):
    """Authenticate the direct check child for classifier evidence, not admission."""
    global CONTRACT_CONTEXT
    stage=Path(__file__).absolute().parents[2]/'.cache/v0.5.1-s4'
    authority=json.loads((stage/'leader/authorization.json').read_text())
    called=previous.read_bound(called_path,called_sha256)
    role=called['role'];require(role in ('builder','reviewer'),'R022 contract role')
    spec=authority['r022']['invocations'][role+'_check']
    require(str(called_path)==spec['called_path'] and called['run_id']==NEW_CHECK and called['r022_recovery_sha256']==authority['r022']['control_sha256'],'R022 contract authority')
    used=json.loads(Path(spec['used_path']).read_text())
    from proc_identity_v11 import process_identity
    driver=process_identity(os.getppid());governor=process_identity(int(os.environ['HP_BASELINE_GOVERNOR_PID']))
    require(driver is not None and governor is not None and driver['ppid']==governor['pid'] and governor['starttime']==int(os.environ['HP_BASELINE_GOVERNOR_STARTTIME']),'R022 contract live driver/governor chain')
    require(used['invocation_pid']==governor['pid'] and used['role']==role and used['run_id']==NEW_CHECK,'R022 contract actual outer invocation')
    CONTRACT_CONTEXT={'called':called,'pid':os.getpid(),'outer_pid':governor['pid'],'driver_pid':os.getppid(),'called_sha256':called_sha256,'classifier_only':True}
    return CONTRACT_CONTEXT

def sparse_charge(path,status,parent_fd,name):
    """Return only the authorized charged size; ordinary files keep old logical size."""
    claim=CURRENT_CLAIM if CURRENT_CLAIM is not None else CONTRACT_CONTEXT
    if claim is None or claim['called']['run_id']!=NEW_CHECK or claim['pid']!=os.getpid():return None
    source=Path(__file__).absolute().parents[2];stage=source/'.cache/v0.5.1-s4';role=claim['called']['role']
    root=stage/role/'run-r018-build-001/build-output/source-B/.cache/r022-benchmark-tests'/NEW_CHECK
    path=Path(path)
    if not path.is_relative_to(root):return None
    relative=path.relative_to(root).parts
    if len(relative)<2 or not relative[0].startswith('synthetic-'):return None
    sizes={('oversize.stdout',):(2147483649,),('sample-01','server.stderr'):(1073741824,),('sample-02','measurement.stdout'):(1073741823,1073741824,1073741825)}
    permitted=sizes.get(relative[1:])
    if permitted is None:return None
    test=root.parents[2]/'tests/benchmark_runner_tests.py'
    authority=json.loads((stage/'leader/authorization.json').read_text());grant=authority['r022']
    require(original.sha(test)==grant['benchmark_test_sha256'],'R022 frozen sparse-test identity')
    require(claim['called']['r022_recovery_sha256']==grant['control_sha256'],'R022 sparse current authority')
    if not stat.S_ISREG(status.st_mode) or status.st_nlink!=1 or status.st_size not in permitted or status.st_blocks*512>65536:return None
    fd=os.open(name,os.O_RDONLY|os.O_NONBLOCK|os.O_NOFOLLOW,dir_fd=parent_fd)
    try:
        opened=os.fstat(fd);after=os.stat(name,dir_fd=parent_fd,follow_symlinks=False)
        fields=lambda value:(value.st_dev,value.st_ino,value.st_mode,value.st_nlink,value.st_size,value.st_blocks)
        require(fields(status)==fields(opened)==fields(after),'R022 sparse identity drift')
        charged=max(opened.st_blocks*512,4096)
        evidence={'path':str(path),'dev':opened.st_dev,'inode':opened.st_ino,'nlink':opened.st_nlink,'blocks_before':status.st_blocks,'blocks_after':after.st_blocks,'logical_bytes':opened.st_size,'allocated_bytes':opened.st_blocks*512,'charged_bytes':charged,'reason':'R022 exact frozen synthetic log-limit fixture'}
        if evidence not in SPARSE_EVIDENCE:
            require(len(SPARSE_EVIDENCE)<128,'R022 sparse evidence capacity');SPARSE_EVIDENCE.append(evidence)
        return charged
    finally:os.close(fd)

def canonical(value):return previous.canonical(value)

def recovery(stage,authorization):
    previous.recovery(stage,authorization)
    grant=authorization['r022'];bound=previous.read_bound(grant['control_path'],grant['control_sha256'])
    require(bound['schema']=='r022-recovery-v1' and bound['new_run_id']==NEW_CHECK and bound['new_seconds']==60 and bound['new_kind']=='ctest','R022 recovery shape')
    ledger=json.loads((stage/'builder/ledger.json').read_text())
    for key,run,status in (('old_builder_check_row','run-r018-check-001','invalid'),('builder_build_row','run-r018-build-001','valid')):
        rows=[r for r in ledger['runs'] if r['run_id']==run]
        require(len(rows)==1 and canonical(rows[0])==bound[key+'_sha256'] and rows[0]==bound[key] and rows[0]['status']==status,'R022 exact historical row')
        require(rows[0]['byte_classification_status']=='verified' and rows[0]['accounting_errors']==[],'R022 historical accounting')
    require(bound['old_builder_check_row']['charged_seconds']==bound['old_actual_charge_seconds'],'R022 retained actual fee')
    for key,value in bound.items():
        if key.endswith('_path'):require(original.sha(Path(value))==bound[key[:-5]+'_sha256'],'R022 bound artifact '+key)
    return bound

def invocation_key(role,run):return role+'_check' if run==NEW_CHECK and role in ('builder','reviewer') else None

def validate_claim(stage,role,run,authorization):
    key=invocation_key(role,run)
    if key is None:return
    spec=authorization['r022']['invocations'][key];claim=CURRENT_CLAIM
    require(claim is not None and claim['pid']==os.getpid() and claim['path']==spec['used_path'],'R022 current credential')
    used=json.loads(Path(spec['used_path']).read_text());called=claim['called']
    require(used['invocation_pid']==os.getpid() and used['role']==role and used['run_id']==run and original.sha(Path(spec['called_path']))==claim['called_sha256'],'R022 actual invocation')
    require(called['attempt']=='attempt001' and called['r022_recovery_sha256']==authorization['r022']['control_sha256'] and called['r021_recovery_sha256']==authorization['r021']['control_sha256'] and called['r020_control_debt_sha256']==authorization['r020']['control_debt_sha256'] and called['control_debt_sha256']==authorization['r019']['control_debt_sha256'],'R022 recovery authority')

def valid_row(stage,role,run):
    if run!=NEW_CHECK:return original.valid_row(stage,role,run)
    ledger=json.loads((stage/role/'ledger.json').read_text());rows=[r for r in ledger['runs'] if r['run_id']==run]
    require(len(rows)==1,'R022 prerequisite unique');row=rows[0]
    require(row['kind']=='ctest' and type(row['reserved_seconds']) in (int,float) and row['reserved_seconds']==60 and row['status']=='valid' and row['byte_classification_status']=='verified' and row['accounting_errors']==[] and row['output']==str(stage/role/run),'R022 prerequisite status')
    output=stage/role/run;cleanup=json.loads((output/'cleanup.json').read_text());protection=json.loads((output/'protection-after.json').read_text());receipt=json.loads((output/'check-receipt.json').read_text())
    require(cleanup['complete'] and not any(cleanup[k] for k in ('forced','errors','remaining','unknown')) and protection['match'] and receipt['status']=='valid','R022 prerequisite evidence')
    authority=json.loads((stage/'leader/authorization.json').read_text());spec=authority['r022']['invocations'][role+'_check']
    require(stat.S_ISREG(Path(spec['used_path']).lstat().st_mode) and row['r019_used_path']==spec['used_path'] and row['r019_called_sha256']==original.sha(Path(spec['called_path'])),'R022 prerequisite invocation')
    require(receipt['build_receipt_sha256']==original.sha(stage/role/'run-r018-build-001/build-output/build-receipt.json'),'R022 check build binding')
    return row

def new_debt(stage,role,authorization):return previous.new_debt(stage,role,authorization)

def check_slot(stage,role,run,kind,seconds,authorization):
    allowed=authorization['r025']['stage_allowlist'];matches=[i for i,s in enumerate(allowed) if (role,run,kind,seconds)==(s['role'],s['run_id'],s['kind'],s['seconds'])]
    require(len(matches)==1 and type(seconds) in (int,float) and math.isfinite(seconds),'R025 exact five-slot scope')
    bound=recovery(stage,authorization);oldreview=previous.recovery(stage,authorization)
    previous.successful_check(stage,'builder',authorization);previous.successful_check(stage,'reviewer',authorization)
    for owner in ('builder','reviewer'):
        rows=json.loads((stage/owner/'ledger.json').read_text())['runs']
        for row in rows:
            if row['run_id'].startswith(('run-r018-','run-r019-','run-r021-','run-r022-')):
                precise=(owner=='builder' and canonical(row)==bound['old_builder_check_row_sha256']) or (owner=='reviewer' and canonical(row)==oldreview['old_ledger_row_sha256'])
                require(row['status']=='valid' or precise,'R022 first failure stops')
        for spec in authorization['r022']['invocations'].values():
            if spec['role']!=owner:continue
            used=Path(spec['used_path']);called=Path(spec['called_path'])
            try:used.lstat()
            except FileNotFoundError:
                try:called.lstat()
                except FileNotFoundError:continue
                raise ValueError('R022 called committed without used')
            if owner==role and run==NEW_CHECK:validate_claim(stage,role,run,authorization)
            else:valid_row(stage,owner,NEW_CHECK)
    for spec in allowed[:matches[0]]:valid_row(stage,spec['role'],spec['run_id'])
    require(not any(r['run_id']==run for r in json.loads((stage/role/'ledger.json').read_text())['runs']),'R022 once')
    if run==NEW_CHECK:validate_claim(stage,role,run,authorization);original.valid_row(stage,role,'run-r018-build-001')
    elif kind=='smoke':valid_row(stage,role,NEW_CHECK)
    rows=json.loads((stage/role/'ledger.json').read_text())['runs'];charged=0
    for row in rows:
        amount=row.get('charged_seconds',row['reserved_seconds']);require(type(amount) in (int,float) and math.isfinite(amount) and amount>=0,'R022 unknown charge');charged+=amount
    complete={r['run_id'] for r in rows if r['status']=='valid'}
    future=sum(s['seconds'] for s in allowed if s['role']==role and s['run_id'] not in complete)
    future+=40+(135 if role=='reviewer' else 0) # Reserved future plan; not execution authority.
    require(charged+previous.old_debt(stage,role,authorization)+new_debt(stage,role,authorization)+future<=authorization['per_role_dynamic_seconds'],'R022 complete future plan')
    return True

def capacity(stage,role,authorization,scanner,fixture_link):
    upper=previous.capacity(stage,role,authorization,scanner,fixture_link)
    fixtures=(stage/role/'run-r018-build-001/build-output/build-B/test-tmp',)
    def measure(path,future=False):return measure_declared_root(path,scanner,fixture_link,fixtures,future)['bytes']
    new=measure(stage/role/NEW_CHECK,future=True)
    old=measure(stage/role/'run-r018-check-001',future=True);smoke=measure(stage/role/'run-r018-smoke-001',future=True)
    require(old+smoke+new<=authorization['r018']['check_smoke_output_bytes'],'R022 original check_smoke group')
    temp=stage/role/'run-r018-build-001/build-output/source-B/.cache/r022-benchmark-tests'/NEW_CHECK
    require(measure(temp,future=True)<=authorization['r022']['benchmark_temp_limit_bytes'],'R022 exact benchmark subtree8MiB')
    source=Path(__file__).parent
    size=sum(measure(source/name) for name in ('localize_v19.py','budget_v17.py','r022_admission.py','check_r022.py','ctest_overlay_r022.py','test_r022.py'))
    for path in (stage/role/'cache').iterdir():
        if path.name.startswith('r022') or path.name.startswith('r025'):size+=measure(path)
    size+=measure(Path(authorization['r022']['control_path']))
    for spec in authorization['r022']['invocations'].values():size+=measure(Path(spec['called_path']),future=True)
    require(size<=authorization['r022']['per_role_output_bytes'],'R022 tools4MiB')
    total=upper+size+new
    planned=authorization['r018']['role_output_high_water_bytes'][role]+authorization['r018'][role+'_output_bytes']+sum(authorization[key]['per_role_output_bytes'] for key in ('r019','r020','r021','r022'))+8*1024**2
    require(planned<=authorization['per_role_output_bytes'] and total+8*1024**2<=authorization['per_role_output_bytes'],'R022 complete capacity')
    return total

def prepare_environment(args,reservation,repository,environment,identity):
    if args.run_id!=NEW_CHECK:return original.prepare_environment(args,reservation,repository,environment,identity)
    require(identity is not None,'R022 governor identity');stage=reservation.root;cache=stage/args.role/'cache/r022'
    primary=stage/args.role/'cache/request-boundaries-r018';build=stage/args.role/'run-r018-build-001/build-output'
    entry=cache/'check_r022.py';manifest=cache/'driver-inputs.json'
    expected={'--package-root':str(primary),'--build-output':str(build),'--output-root':str(reservation.output),
              '--governance-test':str(stage/args.role/'cache/r018/test_r018_governance.py'),
              '--governance-test-sha256':original.sha(stage/args.role/'cache/r018/test_r018_governance.py'),
              '--governance-module-sha256':original.sha(stage/args.role/'cache/r018/r018_admission.py'),
              '--driver-manifest':str(manifest),'--driver-manifest-sha256':original.sha(manifest)}
    if args.role=='reviewer':
        bundle=stage/args.role/'cache/r018/verification-bundle-001';contract=cache/'contract-bundle'
        expected.update({'--verification-bundle':str(bundle),'--verification-bundle-sha256':original.sha(bundle/'inputs.json'),
                         '--contract-bundle':str(contract),'--contract-bundle-sha256':original.sha(contract/'inputs.json')})
    require(args.command[:3]==['/usr/bin/python3','-I',str(entry)] and len(args.command[3:])==2*len(expected) and dict(zip(args.command[3::2],args.command[4::2]))==expected,'R022 exact driver command')
    available={line.split(':',1)[0]:line.split(':',1)[1].strip() for line in Path('/proc/meminfo').read_text().splitlines()}
    require(int(available.get('MemAvailable','0 kB').split()[0])*1024>=1024**3,'R022 available memory below1GiB')
    require(original.shutil.disk_usage(stage).free>=4*1024**3 and original.resource.getrlimit(original.resource.RLIMIT_NOFILE)[0]>=1024,'R022 disk/nofile gate')
    environment.update(HP_BASELINE_ROLE=args.role,HP_BASELINE_RUN=args.run_id,HP_BASELINE_OUTPUT_ROOT=str(reservation.output),
                       HP_BASELINE_PACKAGE_SHA256=original.sha(primary/'inputs-lock.json'),HP_BASELINE_BUILD_RECEIPT_SHA256=original.sha(build/'build-receipt.json'),
                       HP_BASELINE_GOVERNOR_PID=str(identity['pid']),HP_BASELINE_GOVERNOR_STARTTIME=str(identity['starttime']),
                       HP_BASELINE_WORK_DEADLINE=str(reservation.started+args.seconds-7),HP_BASELINE_CLEANUP_DEADLINE=str(reservation.started+args.seconds),
                       HP_R022_CALLED_PATH=str(authorization_spec(stage,args.role)['called_path']),HP_R022_CALLED_SHA256=CURRENT_CLAIM['called_sha256'])

def authorization_spec(stage,role):return json.loads((stage/'leader/authorization.json').read_text())['r022']['invocations'][role+'_check']
