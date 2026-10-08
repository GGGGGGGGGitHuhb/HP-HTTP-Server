"""R019窄容量/完整计划/一次调用接缝；测量包不变。"""
import hashlib
import json
import math
import os
from pathlib import Path
import stat
import r018_admission as original
from r016.r015_admission import control_debt as old_debt, read_bound
from r019_capacity import measure_declared_root

CURRENT_CLAIM = None
require = original.require
record_build_artifact = original.record_build_artifact


def new_debt(stage, role, authorization):
    grant = authorization['r019']
    value = read_bound(grant['control_debt_path'], grant['control_debt_sha256'])
    require(value['schema'] == 'r019-control-debt-v1' and value['role'] == 'builder' and
            type(value['debt_seconds']) is int and value['debt_seconds'] == 90 and
            value['meaning'] == 'conservative_limit_not_measured', 'R019 conservative debt')
    failure = read_bound(value['old_failure_path'], value['old_failure_sha256'])
    require(value['old_failure_sha256'] == grant['old_failure_sha256'] and failure['budget_settlement'] == 'unknown', 'R019 historical unknown binding')
    require(value['old_ledger_sha256'] == failure['original_ledger_sha256'] and failure['run_id'] == value['old_run_id'] and not failure['compiler_started'], 'R019 old ledger provenance')
    require(original.sha(Path(value['old_command_path'])) == value['old_command_sha256'], 'R019 old command binding')
    return 90 if role == 'builder' else 0


def invocation_key(role, run):
    if run == 'run-r019-check-001':
        return role + '_check'
    if role == 'builder' and run == 'run-r018-build-001':
        return 'builder_build'
    return None


def validate_claim(stage, role, run, authorization):
    key = invocation_key(role, run)
    if key is None:
        return
    expected = authorization['r019']['invocations'][key]
    require(CURRENT_CLAIM is not None and CURRENT_CLAIM['pid'] == os.getpid() and
            CURRENT_CLAIM['path'] == expected['used_path'], 'R019 current one-use credential')
    used = json.loads(Path(expected['used_path']).read_text())
    require(used['invocation_pid'] == os.getpid() and used['role'] == role and used['run_id'] == run,
            'R019 current credential identity')
    require(original.sha(Path(expected['called_path'])) == CURRENT_CLAIM['called_sha256'], 'R019 called receipt drift')
    called = CURRENT_CLAIM['called']
    require(called['attempt'] == expected['attempt'] and called['control_debt_sha256'] == authorization['r019']['control_debt_sha256'], 'R019 called authority')


def successful_check(stage, role):
    ledger = json.loads((stage / role / 'ledger.json').read_text())
    rows = [row for row in ledger['runs'] if row['run_id'] == 'run-r019-check-001']
    require(len(rows) == 1, 'R019 prerequisite check')
    row = rows[0]
    require(row['kind'] == 'selfcheck' and type(row['reserved_seconds']) in (int,float) and row['reserved_seconds'] == 20 and
            row['status'] == 'valid' and row['byte_classification_status'] == 'verified' and row['accounting_errors'] == [], 'R019 prerequisite invalid')
    used_path = stage / role / 'cache/r019-check-used-001.json'
    called_path = stage / 'leader' / ('r019-' + role + '-check-called-001.json')
    require(stat.S_ISREG(used_path.lstat().st_mode) and row.get('r019_used_path') == str(used_path) and
            row.get('r019_called_sha256') == original.sha(called_path), 'R019 prerequisite invocation receipt')
    cleanup = json.loads((stage / role / row['run_id'] / 'cleanup.json').read_text())
    require(cleanup['complete'] and not any(cleanup[k] for k in ('forced','errors','remaining','unknown')), 'R019 prerequisite cleanup')


def check_slot(stage, role, run, kind, seconds, authorization):
    if not (run.startswith('run-r018-') or run == 'run-r019-check-001'):
        return original.check_slot(stage, role, run, kind, seconds, authorization)
    grant = authorization['r019']
    require(grant['revision'] == 2 and grant['no_automatic_retries'], 'R019 authority')
    require(type(seconds) in (int,float) and math.isfinite(seconds), 'R019 exact numeric deadline')
    validate_claim(stage, role, run, authorization)
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
            except FileNotFoundError: continue
            require(stat.S_ISREG(metadata.st_mode), 'R019 used type')
            if owner == role and specification['run_id'] == run:
                validate_claim(stage, role, run, authorization)
            else:
                matching = [row for row in ledger['runs'] if row['run_id'] == specification['run_id']]
                require(len(matching) == 1 and matching[0]['status'] == 'valid' and matching[0].get('r019_used_path') == str(used) and
                        matching[0].get('r019_called_sha256') == original.sha(Path(specification['called_path'])), 'R019 prior invocation unknown/binding')
    if run == 'run-r019-check-001':
        require(role in ('builder','reviewer') and kind == 'selfcheck' and seconds == 20, 'R019 precise check20')
        ledger = json.loads((stage / role / 'ledger.json').read_text())
        require(not any(row['run_id'] == run for row in ledger['runs']), 'R019 check once')
        if role == 'reviewer': successful_check(stage, 'builder')
    else:
        successful_check(stage, 'builder'); successful_check(stage, 'reviewer')
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
    require(charged + old_debt(stage,role,authorization) + new_debt(stage,role,authorization) + future <= authorization['per_role_dynamic_seconds'], 'R019 full future plan')
    return True


def capacity(stage, role, authorization, scanner, fixture_link):
    fixture_roots = (stage / role / 'run-r018-build-001/build-output/build-B/test-tmp',)
    def measure(path, future=False):
        return measure_declared_root(path, scanner, fixture_link, fixture_roots, future)['bytes']
    for suffix in ('build-001','check-001','smoke-001','decode-001','boundary-01','boundary-02','boundary-03'):
        measure(stage / role / ('run-r018-' + suffix), future=True)
    upper = original.capacity(stage,role,authorization,measure)
    shared = Path(__file__).parent
    new_size = sum(measure(shared/name) for name in ('r019_admission.py','r019_capacity.py','budget_v14.py','localize_v16.py','check_r019.py','test_r019.py'))
    for path in (stage / role / 'cache').iterdir():
        if path.name.startswith('r019'):
            new_size += measure(path)
    new_size += measure(stage / role / 'run-r019-check-001', future=True)
    for name in ('r019-control-debt-001.json', 'r019-builder-check-called-001.json','r019-reviewer-check-called-001.json','r019-builder-build-called-002.json'):
        new_size += measure(stage / 'leader' / name, future=name != 'r019-control-debt-001.json')
    grant = authorization['r019']
    require(new_size <= grant['per_role_output_bytes'], 'R019 actual new bytes')
    planned = authorization['r018']['role_output_high_water_bytes'][role] + authorization['r018'][role+'_output_bytes'] + grant['per_role_output_bytes'] + 8*1024**2
    require(planned <= authorization['per_role_output_bytes'], 'R019 complete planned bytes')
    require(upper + new_size + 8*1024**2 <= authorization['per_role_output_bytes'], 'R019 HWM cumulative bytes')
    return upper + new_size


def prepare_environment(args, reservation, repository, environment, identity):
    if args.run_id == 'run-r019-check-001':
        entry = (reservation.root / 'reviewer/cache/r019/check_independent_r019.py') if args.role == 'reviewer' else Path(__file__).with_name('check_r019.py')
        require(args.command == ['/usr/bin/python3','-I',str(entry),'--role',args.role,'--output-root',str(reservation.output)], 'R019 exact check command')
        require(identity is not None, 'R019 governor identity')
        environment.update(HP_R019_ROLE=args.role,HP_R019_OUTPUT_ROOT=str(reservation.output),
                           HP_R019_WORK_DEADLINE=str(reservation.started+args.seconds-7),
                           HP_R019_CLEANUP_DEADLINE=str(reservation.started+args.seconds))
    else:
        original.prepare_environment(args,reservation,repository,environment,identity)
