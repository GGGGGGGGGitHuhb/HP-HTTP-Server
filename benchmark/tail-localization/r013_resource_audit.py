"""R013 audit002: bounded FD-based retained backing-directory verification."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import time
from r012_resource_audit import ROOT, ROLE, canonical, digest, safe_path, attempt, pid_stat, inspect_identity

RUN = 'run-m3-resource-audit-002'
NAME = 'hp-s4-marker-gczbe_tw'
INSTANCE = 's4-434a7e1276534ab1a3b20abd9f9550c4'
DEADLINE = None

def check_deadline():
    if time.monotonic() >= DEADLINE:
        raise TimeoutError('audit002 original work deadline reached')

def metadata(value):
    return {'device': value.st_dev, 'inode': value.st_ino, 'mode': value.st_mode,
            'uid': value.st_uid, 'gid': value.st_gid}

def mount_id(fd):
    check_deadline()
    with open(f'/proc/self/fdinfo/{fd}') as stream:
        text = stream.read(4097)
    if len(text) > 4096:
        raise ValueError('own fdinfo size bound exceeded')
    values = [line.split(':', 1)[1].strip() for line in text.splitlines() if line.startswith('mnt_id:')]
    if len(values) != 1 or not values[0].isdigit() or int(values[0]) <= 0:
        raise ValueError('own fdinfo mnt_id unavailable')
    return int(values[0])

def empty_fd(fd):
    check_deadline()
    with os.scandir(fd) as iterator:
        for index, entry in enumerate(iterator, 1):
            if index > 4096:
                raise ValueError('candidate entry bound exceeded')
            return {'empty': False, 'first_entry': entry.name,
                    'metadata': metadata(os.stat(entry.name, dir_fd=fd, follow_symlinks=False))}
    return {'empty': True}

def inspect_backing(expected):
    parent = os.open(ROLE / 'tmp', os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    child = None
    try:
        parent_before = metadata(os.fstat(parent))
        candidates = []
        with os.scandir(parent) as iterator:
            for number, entry in enumerate(iterator, 1):
                check_deadline()
                if number > 4096:
                    raise ValueError('parent direct entry bound exceeded')
                if entry.name.startswith('hp-s4-marker-'):
                    if len(candidates) >= 128:
                        raise ValueError('parent marker candidate bound exceeded')
                    candidates.append({'name': entry.name,
                        'metadata': metadata(os.stat(entry.name, dir_fd=parent, follow_symlinks=False))})
        if len(candidates) != 1 or candidates[0]['name'] != NAME:
            raise ValueError('missing or unknown marker candidates')
        before = os.stat(NAME, dir_fd=parent, follow_symlinks=False)
        if not stat.S_ISDIR(before.st_mode):
            raise ValueError('candidate is not a nofollow directory')
        child = os.open(NAME, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=parent)
        opened = os.fstat(child)
        if metadata(before) != metadata(opened):
            raise ValueError('candidate replaced while opening')
        if (opened.st_dev, opened.st_ino, opened.st_uid) != (expected['device'], expected['inode'], expected['uid']):
            raise ValueError('audit001/audit002 candidate object drift')
        namespace_before = os.readlink('/proc/self/ns/mnt')
        ids_before = {'parent': mount_id(parent), 'candidate': mount_id(child)}
        first = empty_fd(child)
        last = empty_fd(child)
        after = os.stat(NAME, dir_fd=parent, follow_symlinks=False)
        closed_view = metadata(os.fstat(child))
        ids_after = {'parent': mount_id(parent), 'candidate': mount_id(child)}
        namespace_after = os.readlink('/proc/self/ns/mnt')
        check_deadline()
        stable = (metadata(before) == metadata(after) == closed_view
                  and parent_before == metadata(os.fstat(parent))
                  and ids_before == ids_after and namespace_before == namespace_after)
        return {'candidates': candidates, 'before': metadata(before), 'opened': metadata(opened),
                'after': metadata(after), 'fstat_after': closed_view,
                'parent_before': parent_before, 'parent_after': metadata(os.fstat(parent)),
                'mnt_id_before': ids_before, 'mnt_id_after': ids_after,
                'own_mount_namespace_before': namespace_before, 'own_mount_namespace_after': namespace_after,
                'empty_before': first, 'empty_after': last, 'stable': stable,
                'same_mount_in_current_view': ids_before['parent'] == ids_before['candidate'],
                'safe_current_backing': stable and first['empty'] and last['empty']
                    and ids_before['parent'] == ids_before['candidate'],
                'historical_inode_identity': 'not_recorded',
                'scope': 'audit001/002 observation match; no claim of uninterrupted historical inode continuity'}
    finally:
        try:
            if child is not None:
                os.close(child)
        finally:
            os.close(parent)

def main():
    global DEADLINE
    parser = argparse.ArgumentParser()
    parser.add_argument('--inputs', type=Path, required=True)
    parser.add_argument('--inputs-sha256', required=True)
    args = parser.parse_args()
    if os.geteuid() != 0:
        raise ValueError('root audit route required')
    path = safe_path(args.inputs)
    if digest(path) != args.inputs_sha256:
        raise ValueError('input hash drift')
    inputs = json.loads(path.read_text())
    ledger = json.loads((ROLE / 'ledger.json').read_text())
    running = [item for item in ledger['runs'] if item['status'] == 'running']
    if len(running) != 1 or running[0]['run_id'] != RUN or running[0]['kind'] != 'selfcheck' or running[0]['reserved_seconds'] != 20:
        raise ValueError('audit002 reservation mismatch')
    DEADLINE = running[0]['start_monotonic'] + 13
    check_deadline()
    for binding in inputs['records']:
        records = [item for item in ledger['runs'] if item['run_id'] == binding['run_id']]
        if len(records) != 1 or hashlib.sha256(canonical(records[0])).hexdigest() != binding['sha256']:
            raise ValueError('old ledger record drift')
    for binding in inputs['files']:
        check_deadline()
        item = safe_path(binding['path'])
        if item.exists() != binding['present'] or (binding['present'] and digest(item) != binding['sha256']):
            raise ValueError('frozen evidence drift')
    if inputs['role'] != 'builder' or inputs['run_id'] != RUN:
        raise ValueError('input role/run mismatch')
    old = ROLE / 'run-m3-abba-01'
    smoke = ROLE / 'run-smoke-001'
    supervisor = json.loads((old / 'marker-supervisor.json').read_text())
    plan = json.loads((smoke / 'marker-resource-plan.json').read_text())
    proof = json.loads((smoke / 'root-recovery-proof.json').read_text())
    prior = json.loads((ROLE / 'run-m3-resource-audit-001/resource-audit.json').read_text())
    expected = next(item for item in prior['tmp_direct_metadata']['value'] if item['name'] == NAME)
    historical = (plan['mount_directory'] == str(ROLE / 'tmp' / NAME)
        and plan['instance'] == str(ROLE / 'tmp' / NAME / 'instances' / INSTANCE)
        and proof['plan_instance'] == plan['instance'] and proof['errors'] == []
        and proof['instance_absent_after'] is True and proof['backing_after']['empty'] is True
        and proof['recovery_mount_remaining'] is False and proof['matching_run_processes'] == []
        and {item['pid'] for item in proof['processes'] if item.get('exists') is False} == {22392, 22391}
        and plan['root_identity']['pid'] == 22392 and plan['root_identity']['starttime'] == 5507519)
    m3 = [inspect_identity(supervisor['worker_identity']),
          inspect_identity(json.loads((old / 'process.json').read_text()))]
    m3_parent = attempt(lambda: pid_stat(14891))
    smoke_worker = inspect_identity(plan['root_identity'])
    smoke_parent = attempt(lambda: pid_stat(22391))
    backing = attempt(lambda: inspect_backing(expected))
    conditions = {'m3_recorded_processes_exited_or_reused': all(item['classification'] in ('exited', 'reused') for item in m3),
        'm3_parent_absent_without_starttime': m3_parent['status'] == 'absent',
        'm3_worker_old_reaped': 14892 in supervisor['reaped'],
        'm3_plan_and_driver_absent': all(not (old / name).exists() for name in ('marker-resource-plan.json', 'marker-driver-identity.json')),
        'smoke_historical_recovery_valid': historical,
        'smoke_worker_exited_or_reused': smoke_worker['classification'] in ('exited', 'reused'),
        'smoke_parent_original_exit_historically_proved': historical,
        'current_backing_only_allowed_candidate_and_safe': backing['status'] == 'read' and backing['value']['safe_current_backing']}
    check_deadline()
    result = {'schema': 1, 'role': 'builder', 'run_id': RUN, 'audit_completed': True,
        'conclusion': 'verified_safe' if all(conditions.values()) else 'unknown',
        'inputs_sha256': args.inputs_sha256, 'helper_sha256': digest(Path(__file__)),
        'command_sha256': digest(ROLE / RUN / 'command.json'), 'bindings': inputs,
        'm3_lifecycle': {'processes': m3, 'parent_current': m3_parent},
        'smoke_lifecycle': {'worker': smoke_worker, 'parent_current': smoke_parent,
            'parent_original_exit_evidence': proof['processes'],
            'parent_current_pid_not_owned_without_starttime': True},
        'backing': backing, 'conditions': conditions,
        'static_lifecycle_mapping': inputs['static_lifecycle_mapping'],
        'limits': ['historical_inode_identity=not_recorded', 'no old namespace enumeration',
            'own FD mnt_id proves only current view', 'historical recovery mount false does not alone prove old namespaces gone',
            'no signals, deletion, socket, mount, recursive/global scan; safe empty backing is retained'],
        'resource_prerequisite': 'pending independent actual review' if all(conditions.values()) else 'BLOCKED'}
    with (ROLE / RUN / 'resource-audit.json').open('x') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')

if __name__ == '__main__':
    main()
