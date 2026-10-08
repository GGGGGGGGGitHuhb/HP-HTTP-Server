"""R018精确具名治理；不改变原所有权、信号或结算路径。"""
import hashlib
import json
import math
import os
import resource
import shutil
from pathlib import Path
import stat

PACKAGE_NAME = 'request-boundaries-r018'
KINDS = {'build': ('selfcheck', 90), 'check': ('ctest', 60), 'smoke': ('smoke', 20), 'decode': ('offline', 40)}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    path = Path(path)
    for parent in (path, *path.parents):
        require(not parent.is_symlink(), 'R018 symlink input')
    require(path.is_file(), 'R018 missing input')
    return hashlib.sha256(path.read_bytes()).hexdigest()


def valid_row(stage, role, run):
    path = stage / role / 'ledger.json'
    ledger = json.loads(path.read_text())
    require(ledger['role'] == role, 'R018 prior ledger role')
    rows = [row for row in ledger['runs'] if row['run_id'] == run]
    require(len(rows) == 1, 'R018 prerequisite missing/repeated')
    row = rows[0]
    expected = KINDS[run.split('-')[2]] if run.split('-')[2] in KINDS else ('boundary', 45)
    require((row['kind'], row['reserved_seconds']) == expected and type(row['reserved_seconds']) in (int, float) and math.isfinite(row['reserved_seconds']), 'R018 prior kind/deadline')
    require(row['status'] == 'valid' and row['byte_classification_status'] == 'verified' and
            row['accounting_errors'] == [], 'R018 prerequisite invalid')
    require(row['output'] == str(stage / role / run), 'R018 prerequisite path')
    require(type(row['charged_seconds']) in (int, float) and math.isfinite(row['charged_seconds']) and
            row['charged_seconds'] >= 0, 'R018 prior fee unknown')
    cleanup = json.loads((stage / role / run / 'cleanup.json').read_text())
    require(cleanup['complete'] and not cleanup['forced'] and not cleanup['errors'] and
            not cleanup['remaining'] and not cleanup['unknown'], 'R018 prior cleanup incomplete')
    if run == 'run-r018-build-001':
        require(row['r018_build_receipt_sha256'] == sha(stage / role / run / 'build-output/build-receipt.json'), 'R018 prior receipt SHA drift')
    return row


def check_slot(stage, role, run, kind, seconds, authorization):
    if not run.startswith('run-r018-'):
        require(kind != 'boundary', 'boundary only R018')
        return False
    grant = authorization['r018']
    require(grant['revision'] == 1 and grant['no_automatic_retries'], 'R018 authority')
    fixed = {f'run-r018-{key}-001': value for key, value in KINDS.items()}
    fixed.update({f'run-r018-boundary-{index:02}': ('boundary', 45) for index in range(1, 4)})
    require(run in fixed and (kind, seconds) == fixed[run], 'R018 exact run/kind/deadline')
    require(role in ('builder', 'reviewer'), 'R018 role')
    require(type(seconds) in (int, float) and math.isfinite(seconds), 'R018 seconds type')
    if kind == 'boundary':
        require(role == 'reviewer', 'Builder no boundary samples')
    # 任一角色本轮首次失败停止。没有跨角色借槽或恢复窗口。
    for other in ('builder', 'reviewer'):
        ledger = json.loads((stage / other / 'ledger.json').read_text())
        require(not (other == role and any(row['run_id'] == run for row in ledger['runs'])), 'R018 run once')
        for row in ledger['runs']:
            if row['run_id'].startswith('run-r018-'):
                require(row['status'] == 'valid', 'R018 first failure stops')
    if run == 'run-r018-check-001':
        valid_row(stage, role, 'run-r018-build-001')
    elif run == 'run-r018-smoke-001':
        valid_row(stage, role, 'run-r018-check-001')
    elif kind in ('boundary', 'offline'):
        for other in ('builder', 'reviewer'):
            valid_row(stage, other, 'run-r018-smoke-001')
        number = int(run[-2:]) if kind == 'boundary' else 4
        for prior in range(1, number):
            valid_row(stage, 'reviewer', f'run-r018-boundary-{prior:02}')
    current = json.loads((stage / role / 'ledger.json').read_text())['runs']
    completed = {row['run_id'] for row in current if row['run_id'].startswith('run-r018-')}
    plan = {name: value for name, value in fixed.items() if role == 'reviewer' or value[0] != 'boundary'}
    future = sum(value[1] for name, value in plan.items() if name not in completed)
    consumed = sum(row['charged_seconds'] for row in current if row['run_id'].startswith('run-r018-'))
    require(consumed + future <= grant[role + '_seconds'], 'R018 complete role time plan')
    charged = sum(row.get('charged_seconds', row['reserved_seconds']) for row in current)
    from r016.r015_admission import control_debt
    charged += control_debt(stage, role, authorization)
    require(charged + future <= authorization['per_role_dynamic_seconds'], 'R018 complete global time plan')
    require(grant['role_output_high_water_bytes'][role] + grant[role + '_output_bytes'] + 8 * 1024**2 <= authorization['per_role_output_bytes'], 'R018 complete bytes plan')
    return True


def capacity(stage, role, authorization, file_bytes):
    grant = authorization.get('r018')
    if not grant:
        return None
    role_root = stage / role
    source = Path(__file__).parent / 'request-boundaries'
    shared_governance = sum(file_bytes(Path(__file__).parent / name) for name in ('r018_admission.py', 'budget_v13.py', 'localize_v15.py'))
    groups = {'build': file_bytes(source) + shared_governance, 'check_smoke': 0, 'decode': 0, 'O1': 0, 'B': 0, 'O2': 0}
    for path in (role_root / 'cache').iterdir():
        if path.name.startswith('r018') or path.name == PACKAGE_NAME:
            groups['build'] += file_bytes(path)
    for suffix, category in [('build-001', 'build'), ('check-001', 'check_smoke'), ('smoke-001', 'check_smoke'),
                              ('decode-001', 'decode'), ('boundary-01', 'O1'), ('boundary-02', 'B'), ('boundary-03', 'O2')]:
        path = role_root / ('run-r018-' + suffix)
        if path.exists():
            groups[category] += file_bytes(path)
    for key, cap in [('build', grant['build_output_bytes']), ('check_smoke', grant['check_smoke_output_bytes']),
                     ('decode', grant['decode_output_bytes']), ('O1', grant['o_sample_output_bytes']),
                     ('B', grant['b_sample_output_bytes']), ('O2', grant['o_sample_output_bytes'])]:
        require(groups[key] <= cap, 'R018 category capacity: ' + key)
    total = sum(groups.values())
    require(total <= grant[role + '_output_bytes'], 'R018 role new capacity')
    upper = grant['role_output_high_water_bytes'][role] + total
    require(upper + 8 * 1024 * 1024 <= authorization['per_role_output_bytes'], 'R018 historical highwater')
    return upper


def prepare_environment(args, reservation, repository, environment, identity):
    if not args.run_id.startswith('run-r018-'):
        return
    require(identity is not None, 'R018 governor identity')
    stage = reservation.root
    primary = stage / args.role / 'cache' / PACKAGE_NAME
    build = stage / args.role / 'run-r018-build-001/build-output'
    relocated = build / 'relocated-package'
    package = primary if args.run_id in ('run-r018-build-001', 'run-r018-check-001') else relocated
    expected = 'build_package.py' if args.kind == 'selfcheck' else ('check_package.py' if args.kind == 'ctest' else ('decode_boundaries.py' if args.kind == 'offline' else 'execute_sample.py'))
    entry = stage / args.role / 'cache/r018/decode_boundaries.py' if args.kind == 'offline' and args.role == 'reviewer' else package / expected
    require(args.command[:3] == ['/usr/bin/python3', '-I', str(entry)], 'R018 exact isolated script')
    pairs = list(zip(args.command[3::2], args.command[4::2]))
    require(len(args.command[3:]) == 2 * len(pairs) and len({key for key, value in pairs}) == len(pairs), 'R018 duplicate/missing CLI')
    supplied = dict(pairs)
    require(supplied.get('--output-root') == str(reservation.output / 'build-output' if args.kind == 'selfcheck' else reservation.output), 'R018 output route')
    if args.kind == 'selfcheck':
        receipt = stage / args.role / 'run-r015-build-001/build-output/build-receipt.json'
        require(supplied == {'--package-root': str(primary), '--output-root': str(reservation.output / 'build-output'),
            '--server-o-receipt': str(receipt), '--server-o-receipt-sha256': sha(receipt)}, 'R018 exact build CLI')
    elif args.kind == 'ctest':
        test = stage / args.role / 'cache/r018/test_r018_governance.py'
        expected_check = {'--package-root': str(primary), '--build-output': str(build), '--output-root': str(reservation.output),
            '--governance-test': str(test), '--governance-test-sha256': sha(test),
            '--governance-module-sha256': sha(Path(__file__))}
        if args.role == 'reviewer':
            bundle = stage / args.role / 'cache/r018/verification-bundle-001'
            expected_check.update({'--verification-bundle': str(bundle), '--verification-bundle-sha256': sha(bundle / 'inputs.json')})
        require(supplied == expected_check, 'R018 exact check CLI')
    elif args.kind == 'offline':
        require(supplied == {'--role': args.role, '--output-root': str(reservation.output), '--build-output': str(build)}, 'R018 exact decode CLI')
    if args.kind in ('smoke', 'boundary'):
        require(supplied == {'--role': args.role, '--run-id': args.run_id, '--output-root': str(reservation.output), '--build-output': str(build)}, 'R018 exact sample CLI')
    if args.kind == 'ctest' and args.role == 'reviewer':
        bundle = stage / args.role / 'cache/r018/verification-bundle-001'
        require(supplied['--verification-bundle'] == str(bundle) and supplied['--verification-bundle-sha256'] == sha(bundle / 'inputs.json'), 'Reviewer independent fixture bundle')
    memory = {}
    for line in Path('/proc/meminfo').read_text().splitlines():
        key, value = line.split(':', 1)
        if key == 'MemAvailable': memory[key] = int(value.split()[0]) * 1024
    require(memory.get('MemAvailable', 0) >= 1024**3, 'R018 available memory below 1GiB')
    require(shutil.disk_usage(stage).free >= 4 * 1024**3 and resource.getrlimit(resource.RLIMIT_NOFILE)[0] >= 1024, 'R018 disk/nofile gate')
    environment.update(HP_BASELINE_ROLE=args.role, HP_BASELINE_RUN=args.run_id,
                       HP_BASELINE_OUTPUT_ROOT=str(reservation.output),
                       HP_BASELINE_PACKAGE_SHA256=sha(package / 'inputs-lock.json'),
                       HP_BASELINE_GOVERNOR_PID=str(identity['pid']),
                       HP_BASELINE_GOVERNOR_STARTTIME=str(identity['starttime']),
                       HP_BASELINE_WORK_DEADLINE=str(reservation.started + args.seconds - 7),
                       HP_BASELINE_CLEANUP_DEADLINE=str(reservation.started + args.seconds))
    if args.kind != 'selfcheck':
        environment['HP_BASELINE_BUILD_RECEIPT_SHA256'] = sha(build / 'build-receipt.json')


def record_build_artifact(args, reservation, environment):
    if not args.run_id.startswith('run-r018-'):
        return
    reservation.record['r018_package_sha256'] = environment['HP_BASELINE_PACKAGE_SHA256']
    if args.run_id == 'run-r018-build-001':
        receipt = reservation.output / 'build-output/build-receipt.json'
        reservation.record['r018_build_receipt_sha256'] = sha(receipt)
