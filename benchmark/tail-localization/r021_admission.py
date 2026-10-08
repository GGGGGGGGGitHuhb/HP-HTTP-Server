"""R021: exact settled Reviewer failure recovery; immutable old history retained."""
import copy
import hashlib
import json
import math
import os
import stat
from pathlib import Path
import r020_admission as previous
from r016.r015_admission import control_debt as old_debt, read_bound
from r019_capacity import measure_declared_root
original=previous.original
require=original.require
CURRENT_CLAIM=None
record_build_artifact=original.record_build_artifact
NEW_CHECK='run-r021-reviewer-check-001'

def set_claim(value):
    global CURRENT_CLAIM
    CURRENT_CLAIM=value

def get_claim():return CURRENT_CLAIM

def canonical(value):return hashlib.sha256(json.dumps(value,sort_keys=True,separators=(',',':')).encode()).hexdigest()

def recovery(stage,authorization):
    grant=authorization['r021'];bound=read_bound(grant['control_path'],grant['control_sha256'])
    require(bound['schema']=='r021-recovery-v1' and bound['new_run_id']==NEW_CHECK and bound['new_kind']=='selfcheck' and bound['new_seconds']==20 and bound['max_attempts']==1 and bound['no_automatic_retries'],'R021 recovery schema')
    for owner,status,key in (('builder','valid','builder_valid_ledger_row'),('reviewer','invalid','old_ledger_row')):
        ledger=json.loads((stage/owner/'ledger.json').read_text());rows=[r for r in ledger['runs'] if r['run_id']=='run-r019-check-001']
        require(len(rows)==1 and canonical(rows[0])==bound[key+'_sha256'] and rows[0]==bound[key] and rows[0]['status']==status,'R021 exact historical row')
        require(rows[0]['byte_classification_status']=='verified' and rows[0]['accounting_errors']==[],'R021 historical settlement')
    for key in ('old_receipt','old_called','old_used','old_seal','old_failure','builder_valid_receipt'):
        read_bound(bound[key+'_path'],bound[key+'_sha256'])
    require(original.sha(Path(bound['reviewer_report_path']))==bound['reviewer_report_sha256'],'R021 bound report')
    old=bound['old_ledger_row']
    require(old['charged_seconds']==bound['old_actual_charge_seconds'] and old['r019_called_sha256']==bound['old_called_sha256'] and old['r019_used_path']==bound['old_used_path'],'R021 historical invocation binding')
    return bound

def invocation_key(role,run):
    if role=='reviewer' and run==NEW_CHECK:return 'reviewer_check'
    if role=='builder' and run=='run-r018-build-001':return 'builder_build'
    return None

def specifications(authorization):
    return {'builder_check':authorization['r020']['invocations']['builder_check'],
            'reviewer_check':authorization['r021']['invocations']['reviewer_check'],
            'builder_build':authorization['r020']['invocations']['builder_build']}

def validate_claim(stage,role,run,authorization):
    key=invocation_key(role,run)
    if key is None:return
    spec=specifications(authorization)[key];claim=CURRENT_CLAIM
    require(claim is not None and claim['pid']==os.getpid() and claim['path']==spec['used_path'],'R021 current one-use credential')
    used=json.loads(Path(spec['used_path']).read_text());called=claim['called']
    require(used['invocation_pid']==os.getpid() and used['role']==role and used['run_id']==run,'R021 current identity')
    require(original.sha(Path(spec['called_path']))==claim['called_sha256'] and called['attempt']==spec['attempt'],'R021 called binding')
    require(called['control_debt_sha256']==authorization['r019']['control_debt_sha256'] and called['r020_control_debt_sha256']==authorization['r020']['control_debt_sha256'] and called['r021_recovery_sha256']==authorization['r021']['control_sha256'],'R021 exact recovery authority')

def successful_check(stage,role,authorization):
    run='run-r019-check-001' if role=='builder' else NEW_CHECK
    ledger=json.loads((stage/role/'ledger.json').read_text());rows=[r for r in ledger['runs'] if r['run_id']==run]
    require(len(rows)==1,'R021 prerequisite check');row=rows[0];spec=specifications(authorization)[role+'_check']
    require(row['status']=='valid' and row['kind']=='selfcheck' and type(row['reserved_seconds']) in (int,float) and row['reserved_seconds']==20 and row['byte_classification_status']=='verified' and row['accounting_errors']==[],'R021 prerequisite invalid')
    require(stat.S_ISREG(Path(spec['used_path']).lstat().st_mode) and row['r019_used_path']==spec['used_path'] and row['r019_called_sha256']==original.sha(Path(spec['called_path'])),'R021 prerequisite invocation')
    output=stage/role/run;cleanup=json.loads((output/'cleanup.json').read_text());protection=json.loads((output/'protection-after.json').read_text());receipt=json.loads((output/'check-receipt.json').read_text())
    require(cleanup['complete'] and not any(cleanup[k] for k in ('forced','errors','remaining','unknown')) and protection['match'] and receipt['status']=='valid' and receipt['failures']==0 and receipt['errors']==0,'R021 prerequisite evidence')

def new_debt(stage,role,authorization):return previous.new_debt(stage,role,authorization)

def check_slot(stage,role,run,kind,seconds,authorization):
    if not (run.startswith('run-r018-') or run==NEW_CHECK):return previous.check_slot(stage,role,run,kind,seconds,authorization)
    bound=recovery(stage,authorization);require(authorization['r021']['revision']==1 and authorization['r021']['no_automatic_retries'],'R021 authority')
    require(type(seconds) in (int,float) and math.isfinite(seconds),'R021 exact numeric deadline');validate_claim(stage,role,run,authorization)
    for owner in ('builder','reviewer'):
        ledger=json.loads((stage/owner/'ledger.json').read_text())
        for row in ledger['runs']:
            if row['run_id'].startswith(('run-r018-','run-r019-','run-r021-')):
                recovered=owner=='reviewer' and row['run_id']=='run-r019-check-001' and canonical(row)==bound['old_ledger_row_sha256']
                require(row['status']=='valid' or recovered,'R021 first failure stops')
        for spec in specifications(authorization).values():
            if spec['role']!=owner:continue
            used=Path(spec['used_path'])
            try:meta=used.lstat()
            except FileNotFoundError:
                try:Path(spec['called_path']).lstat()
                except FileNotFoundError:continue
                raise ValueError('R021 called committed without used')
            require(stat.S_ISREG(meta.st_mode),'R021 used type')
            if owner==role and spec['run_id']==run:validate_claim(stage,role,run,authorization)
            else:
                matches=[r for r in ledger['runs'] if r['run_id']==spec['run_id']]
                require(len(matches)==1 and matches[0]['status']=='valid' and matches[0]['r019_used_path']==str(used) and matches[0]['r019_called_sha256']==original.sha(Path(spec['called_path'])),'R021 previous invocation invalid')
    if run==NEW_CHECK:
        require(role=='reviewer' and kind=='selfcheck' and seconds==20,'R021 unique check20')
        require(not any(r['run_id']==run for r in json.loads((stage/role/'ledger.json').read_text())['runs']),'R021 check once');successful_check(stage,'builder',authorization)
    else:
        successful_check(stage,'builder',authorization);successful_check(stage,'reviewer',authorization);original.check_slot(stage,role,run,kind,seconds,authorization)
    rows=json.loads((stage/role/'ledger.json').read_text())['runs'];complete={r['run_id'] for r in rows if r['status']=='valid'}
    plan={f'run-r018-{key}-001':value[1] for key,value in original.KINDS.items()}
    if role=='reviewer':plan.update({f'run-r018-boundary-{i:02}':45 for i in range(1,4)});plan[NEW_CHECK]=20
    charged=0
    for row in rows:
        amount=row.get('charged_seconds',row['reserved_seconds']);require(type(amount) in (int,float) and math.isfinite(amount) and amount>=0,'R021 unknown charge');charged+=amount
    require(charged+old_debt(stage,role,authorization)+new_debt(stage,role,authorization)+sum(v for k,v in plan.items() if k not in complete)<=authorization['per_role_dynamic_seconds'],'R021 complete future plan')
    return True

def capacity(stage,role,authorization,scanner,fixture_link):
    upper=previous.capacity(stage,role,authorization,scanner,fixture_link)
    def measure(path,future=False):return measure_declared_root(path,scanner,fixture_link,(),future)['bytes']
    source=Path(__file__).parent;size=sum(measure(source/name) for name in ('r021_admission.py','budget_v16.py','localize_v18.py'))
    for path in (stage/role/'cache').iterdir():
        if path.name.startswith('r021'):size+=measure(path)
    control=Path(authorization['r021']['control_path']);require(control.parent==stage/'leader','R021 control scope');size+=measure(control)
    size+=measure(Path(authorization['r021']['invocations']['reviewer_check']['called_path']),future=True)
    size+=measure(stage/role/NEW_CHECK,future=True)
    require(size<=authorization['r021']['per_role_output_bytes'],'R021 new bytes')
    planned=authorization['r018']['role_output_high_water_bytes'][role]+authorization['r018'][role+'_output_bytes']+authorization['r019']['per_role_output_bytes']+authorization['r020']['per_role_output_bytes']+authorization['r021']['per_role_output_bytes']+8*1024**2
    require(planned<=authorization['per_role_output_bytes'] and upper+size+8*1024**2<=authorization['per_role_output_bytes'],'R021 complete capacity')
    return upper+size

def prepare_environment(args,reservation,repository,environment,identity):
    if args.run_id==NEW_CHECK:
        entry=reservation.root/'reviewer/cache/r021/check_independent_r021.py'
        require(args.command==['/usr/bin/python3','-I',str(entry),'--role','reviewer','--output-root',str(reservation.output)],'R021 exact check argv');require(identity is not None,'R021 governor identity')
        environment.update(HP_R021_ROLE=args.role,HP_R021_OUTPUT_ROOT=str(reservation.output),HP_R021_WORK_DEADLINE=str(reservation.started+args.seconds-7),HP_R021_CLEANUP_DEADLINE=str(reservation.started+args.seconds),HP_R021_EXECUTION_SEAL=CURRENT_CLAIM['called']['execution_seal_path'],HP_R021_EXECUTION_SEAL_SHA256=CURRENT_CLAIM['called']['execution_seal_sha256'])
    else:original.prepare_environment(args,reservation,repository,environment,identity)
