"""R012 bounded read-only audit. Missing ownership evidence remains unknown."""
import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import stat
import time

ROOT = Path(__file__).resolve().parents[2]
ROLE = ROOT / '.cache/v0.5.1-s4/builder'
RUN = 'run-m3-resource-audit-001'

def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':')).encode()

def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def safe_path(path):
    path = Path(path)
    if not path.is_absolute() or '..' in path.parts or not path.is_relative_to(ROOT):
        raise ValueError('input path outside repository')
    for parent in [path, *path.parents]:
        if parent == ROOT.parent:
            break
        if parent.is_symlink():
            raise ValueError('symlink input rejected')
    return path

def attempt(function):
    try:
        return {'status': 'read', 'value': function()}
    except OSError as error:
        return {'status': 'absent' if error.errno in (errno.ENOENT, errno.ESRCH) else 'unknown',
                'type': type(error).__name__, 'errno': error.errno}
    except (ValueError, IndexError) as error:
        return {'status': 'unknown', 'type': type(error).__name__, 'message': str(error)}

def pid_stat(pid):
    text = Path(f'/proc/{pid}/stat').read_text()
    fields = text[text.rfind(')') + 2:].split()
    return {'pid': pid, 'starttime': int(fields[19]), 'state': fields[0], 'ppid': int(fields[1])}

def inspect_identity(identity):
    pid = identity['pid']
    current = attempt(lambda: pid_stat(pid))
    result = {'recorded': identity, 'stat': current}
    if current['status'] != 'read':
        result['classification'] = 'exited' if current['status'] == 'absent' else 'unknown'
        return result
    if current['value']['starttime'] != identity['starttime']:
        result['classification'] = 'reused'
        return result
    result['classification'] = 'same_identity'
    result['namespaces'] = {name: attempt(lambda name=name: os.readlink(f'/proc/{pid}/ns/{name}'))
                            for name in ('mnt', 'pid', 'time')}
    def fds():
        names = []
        with os.scandir(f'/proc/{pid}/fd') as iterator:
            for entry in iterator:
                if len(names) >= 128:
                    raise ValueError('recorded process FD bound exceeded')
                names.append(entry.name)
        return {name: attempt(lambda name=name: os.readlink(f'/proc/{pid}/fd/{name}'))
                for name in sorted(names) if name.isdigit()}
    result['existing_fd_metadata'] = attempt(fds)
    result['identity_after'] = attempt(lambda: pid_stat(pid))
    if result['identity_after']['status'] != 'read' or result['identity_after']['value']['starttime'] != identity['starttime']:
        result['classification'] = 'unknown_changed_during_read'
    return result

def tmp_metadata():
    fd = os.open(ROLE / 'tmp', os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        entries = []
        with os.scandir(fd) as iterator:
            for number, entry in enumerate(iterator, 1):
                if number > 4096:
                    raise ValueError('direct tmp entry bound exceeded')
                if not entry.name.startswith('hp-s4-marker-'):
                    continue
                if len(entries) >= 128:
                    raise ValueError('marker candidate bound exceeded')
                item = os.stat(entry.name, dir_fd=fd, follow_symlinks=False)
                entries.append({'name': entry.name, 'mode': item.st_mode, 'uid': item.st_uid,
                    'gid': item.st_gid, 'device': item.st_dev, 'inode': item.st_ino,
                    'size': item.st_size, 'mtime_ns': item.st_mtime_ns,
                    'symlink': stat.S_ISLNK(item.st_mode), 'ownership': 'unknown'})
        return entries
    finally:
        os.close(fd)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--inputs', type=Path, required=True)
    parser.add_argument('--inputs-sha256', required=True)
    args = parser.parse_args()
    if os.geteuid() != 0:
        raise ValueError('exact root read-only audit route required')
    inputs_path = safe_path(args.inputs)
    if digest(inputs_path) != args.inputs_sha256:
        raise ValueError('audit inputs hash drift')
    inputs = json.loads(inputs_path.read_text())
    if inputs['role'] != 'builder' or inputs['run_id'] != RUN:
        raise ValueError('wrong audit identity')
    ledger = json.loads((ROLE / 'ledger.json').read_text())
    active = [item for item in ledger['runs'] if item['status'] == 'running']
    if len(active) != 1 or active[0]['run_id'] != RUN or active[0]['kind'] != 'selfcheck' or active[0]['reserved_seconds'] != 20:
        raise ValueError('audit reservation missing or ambiguous')
    if time.monotonic() >= active[0]['start_monotonic'] + 13:
        raise ValueError('audit work deadline already reached')
    old = [item for item in ledger['runs'] if item['run_id'] == 'run-m3-abba-01']
    if len(old) != 1 or hashlib.sha256(canonical(old[0])).hexdigest() != inputs['old_record_sha256']:
        raise ValueError('old canonical failure record drift')
    for item in inputs['files']:
        path = safe_path(item['path'])
        if item['present'] != path.exists() or (item['present'] and digest(path) != item['sha256']):
            raise ValueError('frozen audit evidence drift')
    supervisor = json.loads((ROLE / 'run-m3-abba-01/marker-supervisor.json').read_text())
    worker = supervisor['worker_identity']
    if (worker['pid'], worker['starttime']) != (14892, 7804342):
        raise ValueError('old worker identity mismatch')
    identities = [worker, json.loads((ROLE / 'run-m3-abba-01/process.json').read_text())]
    result = {'schema': 1, 'role': 'builder', 'run_id': RUN, 'audit_completed': True,
        'command_sha256': digest(ROLE / RUN / 'command.json'),
        'inputs_sha256': args.inputs_sha256, 'helper_sha256': digest(Path(__file__)),
        'old_record_sha256': inputs['old_record_sha256'], 'artifact_bindings': inputs['files'],
        'processes': [inspect_identity(item) for item in identities],
        'recorded_parent_without_starttime': {'pid': worker['ppid'], 'stat': attempt(lambda: pid_stat(worker['ppid'])),
            'ownership': 'unverified; no recorded starttime; no namespace/FD access'},
        'tmp_direct_metadata': attempt(tmp_metadata),
        'old_supervisor_resources_restored': supervisor['resources_restored'],
        'evidence': ['old worker reaped is recorded; outer cleanup complete is recorded'],
        'inferences': ['current absence or PID reuse does not prove private resource lifecycle closure'],
        'unverifiable': ['old private mount namespace identifier absent',
            'old marker-resource-plan and complete resource inventory absent',
            'parent namespace/FD ownership lacks recorded starttime',
            'prefix/uid/mtime alone do not establish candidate ownership'],
        'conclusion': 'unknown', 'resource_prerequisite': 'BLOCKED',
        'actions': 'read-only; no signals, mount operations, deletion, recursive or global scans'}
    if time.monotonic() >= active[0]['start_monotonic'] + 13:
        raise ValueError('audit work deadline reached')
    processes = result['processes']
    parent_absent = result['recorded_parent_without_starttime']['stat']['status'] == 'absent'
    tmp = result['tmp_direct_metadata']
    prerequisites = {
        'worker_reaped_in_old_supervisor': worker['pid'] in supervisor['reaped'],
        'recorded_identities_exited_or_reused': all(item['classification'] in ('exited', 'reused') for item in processes),
        'recorded_parent_pid_absent': parent_absent,
        'complete_direct_tmp_without_candidates': tmp['status'] == 'read' and not tmp['value'],
        'old_plan_and_driver_record_absent': all(not (ROLE / 'run-m3-abba-01' / name).exists()
            for name in ('marker-resource-plan.json', 'marker-driver-identity.json')),
        'source_lifecycle_chain_sealed': inputs['source_lifecycle_chain']['independently_reviewed']}
    result['lifecycle_prerequisites'] = prerequisites
    result['source_lifecycle_chain'] = inputs['source_lifecycle_chain']
    if all(prerequisites.values()):
        result['conclusion'] = 'verified_safe'
        result['resource_prerequisite'] = 'pending independent actual review'
        result['inferences'].append('no remaining holder under the sealed pre-plan synchronous-child and supervisor-FD lifecycle chain')
        result['unverifiable'] = ['old mount namespace ID absent; safety relies on explicit lifecycle premises rather than mount enumeration']
    output = ROLE / RUN / 'resource-audit.json'
    with output.open('x') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')

if __name__ == '__main__':
    main()
