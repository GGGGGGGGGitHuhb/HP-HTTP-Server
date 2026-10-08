"""R020仅补20债/attempt映射/参数入口与新增2MiB；旧collector不改。"""
import copy
import json
import math
import stat
from pathlib import Path
import r019_admission as previous
from r016.r015_admission import control_debt as old_debt, read_bound
from r019_capacity import measure_declared_root

original = previous.original
require = original.require
CURRENT_CLAIM = None
record_build_artifact = original.record_build_artifact
invocation_key = previous.invocation_key


def set_claim(value):
    global CURRENT_CLAIM
    CURRENT_CLAIM = value
    previous.CURRENT_CLAIM = value


def get_claim(): return CURRENT_CLAIM


def effective_authorization(authorization):
    result = copy.deepcopy(authorization)
    result['r019']['invocations'] = result['r020']['invocations']
    return result


def twenty_debt(stage, role, authorization):
    grant=authorization['r020']
    value=read_bound(grant['control_debt_path'],grant['control_debt_sha256'])
    require(value['schema']=='r020-control-debt-v1' and value['role']=='builder' and
            type(value['debt_seconds']) is int and value['debt_seconds']==20 and
            value['meaning']=='conservative_limit_not_measured','R020 conservative debt')
    failure=read_bound(value['old_failure_path'],value['old_failure_sha256'])
    read_bound(value['old_called_path'],value['old_called_sha256'])
    read_bound(value['old_commands_path'],value['old_commands_sha256'])
    read_bound(value['old_seal_path'],value['old_seal_sha256'])
    require(failure['schema']=='r019-first-invocation-failure-v1' and failure['settlement']=='unknown_no_ledger' and
            failure['actual_charge_seconds'] is None and not failure['used_exists'] and not failure['run_directory_exists'] and
            not failure['collector_started'] and not failure['inner_command_started'] and failure['run_id']=='run-r019-check-001' and
            value['old_failure_sha256']==grant['old_failure_sha256'],'R020 only bound old unknown')
    return 20 if role=='builder' else 0


def new_debt(stage, role, authorization):
    return previous.new_debt(stage,role,authorization)+twenty_debt(stage,role,authorization)


def successful_check(stage,role,authorization):
    ledger=json.loads((stage/role/'ledger.json').read_text())
    rows=[row for row in ledger['runs'] if row['run_id']=='run-r019-check-001']
    require(len(rows)==1,'R020 prerequisite check')
    row=rows[0]
    require(row['kind']=='selfcheck' and type(row['reserved_seconds']) in (int,float) and row['reserved_seconds']==20 and
            row['status']=='valid' and row['byte_classification_status']=='verified' and row['accounting_errors']==[], 'R020 prerequisite invalid')
    specification=authorization['r020']['invocations'][role+'_check']
    used=Path(specification['used_path']);called=Path(specification['called_path'])
    require(stat.S_ISREG(used.lstat().st_mode) and row.get('r019_used_path')==str(used) and
            row.get('r019_called_sha256')==original.sha(called),'R020 exact successful attempt')
    cleanup=json.loads((stage/role/row['run_id']/'cleanup.json').read_text())
    require(cleanup['complete'] and not any(cleanup[k] for k in ('forced','errors','remaining','unknown')),'R020 prerequisite cleanup')


def check_slot(stage, role, run, kind, seconds, authorization):
    if not (run.startswith('run-r018-') or run == 'run-r019-check-001'):
        return previous.check_slot(stage, role, run, kind, seconds, authorization)
    twenty = twenty_debt(stage,role,authorization)
    require(authorization['r020']['revision']==1 and authorization['r020']['no_automatic_retries'],'R020 authority')
    authorization=effective_authorization(authorization)
    previous.CURRENT_CLAIM=CURRENT_CLAIM
    grant = authorization['r019']
    require(grant['revision'] == 2 and grant['no_automatic_retries'], 'R019 authority')
    require(type(seconds) in (int,float) and math.isfinite(seconds), 'R019 exact numeric deadline')
    previous.validate_claim(stage, role, run, authorization)
    if invocation_key(role,run) is not None:
        require(CURRENT_CLAIM['called']['r020_control_debt_sha256']==authorization['r020']['control_debt_sha256'],'R020 invocation 20 debt binding')
    for owner in ('builder','reviewer'):
        ledger = json.loads((stage / owner / 'ledger.json').read_text())
        for row in ledger['runs']:
            if row['run_id'].startswith(('run-r018-','run-r019-')):
                require(row['status'] == 'valid', 'R019/R018 first failure stops')
        for key, specification in grant['invocations'].items():
            if specification['role'] != owner:
                continue
            used = Path(specification['used_path'])
            try: metadata = used.lstat()
            except FileNotFoundError:
                called=Path(specification['called_path'])
                try: called.lstat()
                except FileNotFoundError: continue
                raise ValueError('R020 called committed without used: unknown invocation')
            require(stat.S_ISREG(metadata.st_mode), 'R019 used type')
            if owner == role and specification['run_id'] == run:
                previous.validate_claim(stage, role, run, authorization)
            else:
                matching = [row for row in ledger['runs'] if row['run_id'] == specification['run_id']]
                require(len(matching) == 1 and matching[0]['status'] == 'valid' and matching[0].get('r019_used_path') == str(used) and
                        matching[0].get('r019_called_sha256') == original.sha(Path(specification['called_path'])), 'R019 prior invocation unknown/binding')
    if run == 'run-r019-check-001':
        require(role in ('builder','reviewer') and kind == 'selfcheck' and seconds == 20, 'R019 precise check20')
        ledger = json.loads((stage / role / 'ledger.json').read_text())
        require(not any(row['run_id'] == run for row in ledger['runs']), 'R019 check once')
        if role == 'reviewer': successful_check(stage, 'builder', authorization)
    else:
        successful_check(stage, 'builder', authorization); successful_check(stage, 'reviewer', authorization)
        original.check_slot(stage, role, run, kind, seconds, authorization)
    current = json.loads((stage / role / 'ledger.json').read_text())['runs']
    completed = {row['run_id'] for row in current if row['status'] == 'valid'}
    plan = {f'run-r018-{key}-001': value[1] for key,value in original.KINDS.items()}
    if role == 'reviewer': plan.update({f'run-r018-boundary-{i:02}':45 for i in range(1,4)})
    plan['run-r019-check-001'] = 20
    future = sum(amount for identifier,amount in plan.items() if identifier not in completed)
    charged = 0
    for row in current:
        amount = row.get('charged_seconds', row['reserved_seconds'])
        require(type(amount) in (int,float) and math.isfinite(amount) and amount >= 0, 'R019 accounting amount unknown')
        charged += amount
    require(charged + old_debt(stage,role,authorization) + previous.new_debt(stage,role,authorization) + twenty + future <= authorization['per_role_dynamic_seconds'], 'R019 full future plan')
    return True


def capacity(stage,role,authorization,scanner,fixture_link):
    fixtures=(stage/role/'run-r018-build-001/build-output/build-B/test-tmp',)
    def measure(path,future=False):return measure_declared_root(path,scanner,fixture_link,fixtures,future)['bytes']
    # R019's failed check created no output; the recovered logical directory belongs only to R020.
    for suffix in ('build-001','check-001','smoke-001','decode-001','boundary-01','boundary-02','boundary-03'):
        measure(stage/role/('run-r018-'+suffix),future=True)
    upper=original.capacity(stage,role,authorization,measure)
    old_source=Path(previous.__file__).parent
    nineteen=sum(measure(old_source/name) for name in ('r019_admission.py','r019_capacity.py','budget_v14.py','localize_v16.py','check_r019.py','test_r019.py'))
    for path in (stage/role/'cache').iterdir():
        if path.name.startswith('r019'):nineteen+=measure(path)
    for name in ('r019-control-debt-001.json','r019-builder-check-called-001.json','r019-reviewer-check-called-001.json','r019-builder-build-called-002.json'):
        nineteen+=measure(stage/'leader'/name,future=name!='r019-control-debt-001.json')
    require(nineteen<=authorization['r019']['per_role_output_bytes'],'R019 preserved actual new bytes')
    upper+=nineteen
    source=Path(__file__).parent
    size=sum(measure(source/name) for name in ('r020_admission.py','budget_v15.py','localize_v17.py','check_r020.py','test_r020_entry.py','test_r020_collector.py'))
    for path in (stage/role/'cache').iterdir():
        if path.name.startswith('r020'):size+=measure(path)
    for name in ('r020-control-debt-001.json','r020-protected-source-001.json','r020-builder-check-called-002.json'):
        size+=measure(stage/'leader'/name,future=name.endswith('called-002.json'))
    recovered_output=measure(stage/role/'run-r019-check-001',future=True)
    size+=recovered_output
    grant=authorization['r020']
    require(size<=grant['per_role_output_bytes'],'R020 actual new bytes')
    planned=authorization['r018']['role_output_high_water_bytes'][role]+authorization['r018'][role+'_output_bytes']+authorization['r019']['per_role_output_bytes']+grant['per_role_output_bytes']+8*1024**2
    require(planned<=authorization['per_role_output_bytes'],'R020 complete planned bytes')
    # Original logical check output is classified once, in R020, after the failed call produced none.
    total=upper+size
    require(total+8*1024**2<=authorization['per_role_output_bytes'],'R020 HWM cumulative bytes')
    return total


def prepare_environment(args,reservation,repository,environment,identity):
    if args.run_id=='run-r019-check-001':
        entry=(reservation.root/'reviewer/cache/r020/check_independent_r020.py') if args.role=='reviewer' else Path(__file__).with_name('check_r020.py')
        require(args.command==['/usr/bin/python3','-I',str(entry),'--role',args.role,'--output-root',str(reservation.output)],'R020 exact check command')
        require(identity is not None,'R020 governor identity')
        environment.update(HP_R020_ROLE=args.role,HP_R020_OUTPUT_ROOT=str(reservation.output),
                           HP_R020_WORK_DEADLINE=str(reservation.started+args.seconds-7),
                           HP_R020_CLEANUP_DEADLINE=str(reservation.started+args.seconds),
                           HP_R020_EXECUTION_SEAL=CURRENT_CLAIM['called']['execution_seal_path'],
                           HP_R020_EXECUTION_SEAL_SHA256=CURRENT_CLAIM['called']['execution_seal_sha256'])
    else:original.prepare_environment(args,reservation,repository,environment,identity)
