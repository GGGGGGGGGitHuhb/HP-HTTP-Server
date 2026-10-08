#!/usr/bin/env python3
"""R031 fixed independent executor; GNU time/timeout are the declared outer boundary."""
import time
ENTRY_NS = time.monotonic_ns()
import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import signal
import stat
import subprocess
import sys

sys.dont_write_bytecode = True
REPO = Path(__file__).absolute().parents[2]
STAGE = REPO / '.cache/v0.5.1-revalidation'
APPROVAL = STAGE / 'leader/control/r031-approval-001.json'
APPROVAL_SHA = '3e62437c2a7cd8459de245f823ce45faebf32735b4b0b0526bd92b27d962bdcd'
RESUME_APPROVAL = STAGE / 'leader/control/r032-approval-001.json'
RESUME_APPROVAL_SHA = '58fc5d0af75c13de3ad535705dc1a70ff0691cee4c39df83a037a793436cb934'
RESUME_SLOTS = [('builder', 'build-C', 240), ('builder', 'build-D', 240), ('builder', 'smoke-C', 45), ('builder', 'smoke-D', 45),
                ('reviewer', 'contract', 30), ('reviewer', 'build-C', 240), ('reviewer', 'build-D', 240),
                ('reviewer', 'smoke-C', 45), ('reviewer', 'smoke-D', 45)]
SCENARIOS = ['supervisor-normal-001', 'supervisor-publish-stderr-001', 'supervisor-pre-handshake-001']
DIAGNOSE_SLOTS=[('builder','diagnose-entry',20),('reviewer','diagnose-entry',20),
                ('builder','diagnose-base',45),('builder','diagnose-observe',45),
                ('builder','diagnose-offline',30),('reviewer','diagnose-offline',30)]


def require(value, message):
    if not value:
        raise ValueError(message)


def sha(path):
    result = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1048576), b''):
            result.update(block)
    return result.hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False).encode()


def real_parents(path):
    require(path.is_absolute() and '..' not in path.parts, 'absolute bounded path required')
    for ancestor in reversed(list(path.parents)):
        require(stat.S_ISDIR(ancestor.lstat().st_mode), 'nofollow parent required')


def read_json(path, expected=None):
    path = Path(path)
    real_parents(path)
    before = path.lstat()
    require(stat.S_ISREG(before.st_mode) and before.st_size <= 4194304, 'bounded regular evidence required')
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        opened = os.fstat(descriptor)
        require((before.st_dev, before.st_ino) == (opened.st_dev, opened.st_ino), 'evidence identity changed')
        with os.fdopen(descriptor, 'rb', closefd=False) as stream:
            data = stream.read(4194305)
        after = os.fstat(descriptor)
        require(len(data) <= 4194304 and (opened.st_size, opened.st_mtime_ns) == (after.st_size, after.st_mtime_ns), 'evidence unstable')
        require(expected is None or hashlib.sha256(data).hexdigest() == expected, 'evidence SHA mismatch')
        return json.loads(data)
    finally:
        os.close(descriptor)


def publish(path, value):
    with Path(path).open('x') as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())


def identity(pid):
    try:
        text = Path('/proc', str(pid), 'stat').read_text()
    except OSError as error:
        if error.errno in (errno.ENOENT, errno.ESRCH):
            return None
        raise
    fields = text[text.rfind(')') + 2:].split()
    return {'pid': pid, 'ppid': int(fields[1]), 'pgid': int(fields[2]), 'sid': int(fields[3]),
            'starttime': int(fields[19]), 'state': fields[0]}


def namespace(pid):
    value = os.stat('/proc/' + str(pid) + '/ns/pid')
    return {'dev': value.st_dev, 'inode': value.st_ino}


def same(first, second):
    return second is not None and all(first[key] == second[key] for key in ('pid', 'starttime', 'pgid', 'sid'))


def bytes_under(path, accounting=None, formal_logs=()):
    try:
        root_stat = path.lstat()
    except FileNotFoundError:
        return 0
    require(stat.S_ISDIR(root_stat.st_mode), 'material root nofollow directory required')
    real_parents(path)
    total = 0
    allocated = root_stat.st_blocks * 512
    logs = 0
    new_logs=0
    def visit(fd, directory):
        nonlocal total, allocated, logs, new_logs
        with os.scandir(fd) as entries:
            for entry in entries:
                try:
                    value = entry.stat(follow_symlinks=False)
                except FileNotFoundError:
                    continue
                if stat.S_ISDIR(value.st_mode):
                    allocated += value.st_blocks * 512
                    try:
                        child = os.open(entry.name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
                    except FileNotFoundError:
                        continue  # Compiler temporary directory disappeared; final scan is repeated.
                    try:
                        opened = os.fstat(child)
                        require((opened.st_dev, opened.st_ino) == (value.st_dev, value.st_ino), 'material directory drift')
                        visit(child,directory/entry.name)
                    finally:
                        os.close(child)
                else:
                    require(stat.S_ISREG(value.st_mode) and value.st_nlink == 1, 'regular unique material required')
                    total += value.st_size
                    allocated += value.st_blocks * 512
                    if entry.name.endswith(('.stdout', '.stderr', '.log', '.wall')):
                        if directory/entry.name in formal_logs:new_logs+=value.st_size
                        else:logs += value.st_size
                    require(max(total, allocated) <= 2147483648 and logs <= 67108864 and new_logs<=268435456, 'role storage/log limit')
    descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        visit(descriptor,path)
    finally:
        os.close(descriptor)
    if accounting is not None:
        accounting.update(logical=total,allocated=allocated,logs=logs,formal_logs=new_logs)
    return total


def execute(args):
    diagnostic = args.step == 'diagnose-contract'
    formal=args.step in {step for _,step,_ in DIAGNOSE_SLOTS}
    resume = args.step is not None
    maximum = next((value for role, step, value in RESUME_SLOTS if (role, step) == (args.role, args.step)), None) if resume else 20
    if diagnostic:
        maximum=40
    if formal:
        maximum=next((seconds for role,step,seconds in DIAGNOSE_SLOTS if (role,step)==(args.role,args.step)),None)
    require(not resume or maximum is not None, 'exact nine remaining slots required')
    # R031 has already been consumed by both roles. Its old branch remains history,
    # never a second invocation route in this R032 entry.
    require(resume, 'R031 once consumed; resume requires exact step')
    approval_path, approval_sha = RESUME_APPROVAL, RESUME_APPROVAL_SHA
    if diagnostic:
        approval_path=STAGE/'leader/control/r033-approval-001.json'
        approval_sha=sha(approval_path)
    if formal:
        approval_path=STAGE/'leader/control/r034-approval-001.json'
        approval_sha=sha(approval_path)
    revision='r034' if formal else ('r033' if diagnostic else 'r032')
    run_id = revision+'-' + args.step + '-001'
    output = None
    process = None
    streams = []
    owned = {}
    observations = []
    failures = []
    first = None
    evidence_ok = True
    started = ENTRY_NS
    cleanup_at = started + (maximum - 4) * 1000000000
    work_at = started + (maximum - 10) * 1000000000
    scope_ns = None
    driver_identity = None
    spawn_attempted = False
    closure = []
    trusted_parent = None
    external_logs = []
    preparation_source_bytes=0
    formal_log_paths={STAGE/args.role/('control/r034-diagnose-'+mode+'-driver-001')/name
                      for mode in ('base','observe') for name in ('server.stdout','server.stderr','client.stdout','client.stderr')} if formal else set()
    def material_bytes():
        whole={}
        total = bytes_under(STAGE / args.role,whole,formal_log_paths)
        require(total <= 2147483648, 'role storage limit')
        if diagnostic:
            logical=allocated=preparation_source_bytes
            for path in (STAGE/args.role/'control').iterdir():
                if not path.name.startswith('r033-'):
                    continue
                value=path.lstat()
                if stat.S_ISDIR(value.st_mode):
                    accounting={}
                    bytes_under(path,accounting)
                    logical+=accounting['logical'];allocated+=accounting['allocated']
                else:
                    require(stat.S_ISREG(value.st_mode) and value.st_nlink==1,'preparation regular material')
                    logical+=value.st_size;allocated+=value.st_blocks*512
            require(max(logical,allocated)<=33554432,'R033 new material 32MiB limit')
        if formal:
            logical=allocated=preparation_source_bytes
            raw_logical=raw_allocated=0
            prepare_logical=prepare_allocated=preparation_source_bytes
            other_logical=other_allocated=0
            for path in (STAGE/args.role/'control').iterdir():
                if path.name.startswith(('r033-','r034-')):
                    if path.is_dir():
                        accounting={};bytes_under(path,accounting,formal_log_paths)
                        logical+=accounting['logical'];allocated+=accounting['allocated']
                        if path.name in ('r034-diagnose-base-driver-001','r034-diagnose-observe-driver-001'):
                            log_allocated=sum(leaf.stat().st_blocks*512 for leaf in formal_log_paths if leaf.parent==path and leaf.exists())
                            raw_logical+=accounting['logical']-accounting['formal_logs']
                            raw_allocated+=accounting['allocated']-log_allocated
                        elif path.name.startswith('r033-') or 'diagnose-entry' in path.name or 'preserved' in path.name:
                            prepare_logical+=accounting['logical'];prepare_allocated+=accounting['allocated']
                        else:
                            other_logical+=accounting['logical'];other_allocated+=accounting['allocated']
                    else:
                        value=path.lstat();require(stat.S_ISREG(value.st_mode) and value.st_nlink==1,'unique R034 material')
                        logical+=value.st_size;allocated+=value.st_blocks*512
                        if 'diagnose-' in path.name and 'diagnose-entry' not in path.name:
                            other_logical+=value.st_size;other_allocated+=value.st_blocks*512
                        else:
                            prepare_logical+=value.st_size;prepare_allocated+=value.st_blocks*512
            require(max(logical,allocated)<=536870912,'R034 material 512MiB limit')
            require(max(raw_logical,raw_allocated)<=201326592,'R034 raw material 192MiB limit')
            require(max(prepare_logical,prepare_allocated)<=33554432 and max(other_logical,other_allocated)<=33554432,'R034 preparation/other 32MiB limit')
            require(max(whole['logical'],whole['allocated'])+536870912-max(logical,allocated)<=2147483648,'whole role complete future reservation')
            if args.step=='diagnose-entry':require(max(logical,allocated)<=33554432,'entry material 32MiB limit')
        return total
    driver_output = STAGE / args.role / ('contract-001' if args.step == 'contract' else (args.step.split('-')[1] if args.step.startswith('build-') else args.step + '-001'))
    if diagnostic:
        driver_output=STAGE/args.role/'control/r033-diagnose-contract-driver-001'
    if formal:driver_output=STAGE/args.role/('control/r034-'+args.step+'-driver-001')
    before_term = signal.getsignal(signal.SIGTERM)
    def note(error):
        nonlocal first
        try:
            try:
                message = str(error)
            except BaseException:
                message = 'message unavailable'
            value = {'type': type(error).__name__, 'message': message}
            if first is None:
                first = value
            else:
                failures.append(value)
        except BaseException:
            first = first or {'type': 'ReportingFailure', 'message': 'failed to preserve error'}
    def save(name, value):
        nonlocal evidence_ok
        if output is None:
            return False
        try:
            publish(output / name, value)
            return True
        except BaseException as error:
            evidence_ok = False
            note(error)
            try:
                print('execute_check evidence failed: ' + name, file=sys.stderr)
            except BaseException as second:
                note(second)
            return False
    def register():
        pending = list(owned.values())
        seen = set()
        while pending:
            parent = pending.pop()
            key = (parent['pid'], parent['starttime'])
            if key in seen:
                continue
            seen.add(key)
            current = identity(parent['pid'])
            if current is None:
                continue
            if not same(parent, current):
                # GNU timeout's one permitted setpgid transition is verified while its
                # original source parent still exists; reparent is never this exception.
                source = identity(parent.get('source_parent', -1)) if parent.get('source_parent') else None
                require(source is not None and current['ppid'] == source['pid'] and current['pid'] == parent['pid']
                        and current['starttime'] == parent['starttime'] and current['sid'] == parent['sid']
                        and parent['pgid'] == source['pgid'] and current['pgid'] == current['pid'], 'registered parent unknown')
                observations.append({'stage': 'verified_timeout_group_transition', 'before': dict(parent), 'after': current})
                parent.update(current)
            require(namespace(current['pid']) == scope_ns, 'registered parent namespace unknown')
            try:
                children = Path('/proc', str(parent['pid']), 'task', str(parent['pid']), 'children').read_text().split()
            except OSError as error:
                if error.errno in (errno.ENOENT, errno.ESRCH):
                    continue
                raise
            require(len(children) <= 128, 'owned children bound')
            for child in children:
                value = identity(int(child))
                if value is None:
                    continue
                require(value['ppid'] == parent['pid'] and namespace(value['pid']) == scope_ns, 'child source identity unknown')
                # Descendant timeout creates a nested group. Capture after its group transition;
                # changed registered group is unknown rather than signaling an inferred identity.
                require(len(owned) < 128 or (value['pid'], value['starttime']) in owned, 'owned tree bound')
                k = (value['pid'], value['starttime'])
                if k not in owned:
                    owned[k] = dict(value, source_parent=parent['pid'])
                else:
                    if not same(owned[k], value):
                        previous = owned[k]
                        require(value['pid'] == previous['pid'] and value['starttime'] == previous['starttime']
                                and value['sid'] == previous['sid'] and previous['pgid'] == parent['pgid']
                                and value['pgid'] == value['pid'], 'registered child changed group or identity')
                        observations.append({'stage': 'verified_timeout_group_transition', 'before': dict(previous), 'after': value})
                        previous.update(value)
                pending.append(owned[k])
    def observe():
        rows = []
        for value in list(owned.values()):
            try:
                current = identity(value['pid'])
                state = 'absent' if current is None else ('zombie' if current['state'] == 'Z' else 'alive')
                if current is not None:
                    require(same(value, current) and namespace(current['pid']) == scope_ns, 'post-wait identity unknown')
                rows.append({'registered': value, 'current': current, 'classification': state})
            except BaseException as error:
                note(error)
                rows.append({'registered': value, 'classification': 'unknown'})
        # Inspect the same namespace after wait, including unregistered surviving
        # members of all actually observed groups. Never claim these objects.
        groups = {value['pgid'] for value in owned.values()}
        groups.add(os.getpgrp())
        try:
            count = 0
            with os.scandir('/proc') as entries:
                for entry in entries:
                    if not entry.name.isdecimal():
                        continue
                    count += 1
                    require(count <= 65536 and time.monotonic_ns() < cleanup_at, 'post-wait scan bound/deadline')
                    try:
                        current = identity(int(entry.name))
                        if current is None or current['pgid'] not in groups or current['pid'] == os.getpid():
                            continue
                        if trusted_parent is not None and same(trusted_parent, current):
                            continue
                        require(namespace(current['pid']) == scope_ns, 'post-wait member namespace unknown')
                        if (current['pid'], current['starttime']) not in owned:
                            rows.append({'current': current, 'classification': 'unknown', 'reason': 'unregistered group member; not claimed'})
                            note(ValueError('unregistered surviving group member'))
                    except OSError as error:
                        if error.errno not in (errno.ENOENT, errno.ESRCH):
                            note(error)
                            rows.append({'pid': int(entry.name), 'classification': 'unknown'})
        except BaseException as error:
            note(error)
            rows.append({'classification': 'unknown', 'reason': 'post-wait scan incomplete'})
        observations.append({'at_ns': time.monotonic_ns(), 'namespace': scope_ns, 'rows': rows})
        return rows
    def stop_owned(signum):
        for value in reversed(list(owned.values())):
            try:
                require(time.monotonic_ns() < cleanup_at, 'independent cleanup deadline exhausted')
                current = identity(value['pid'])
                if current is not None:
                    require(same(value, current) and namespace(current['pid']) == scope_ns, 'refuse unknown signal target')
                    require(current['pid'] != os.getpid(), 'refuse own executor signal')
                    if current['state'] != 'Z':
                        os.kill(current['pid'], signum)
            except OSError as error:
                if error.errno not in (errno.ENOENT, errno.ESRCH):
                    note(error)
            except BaseException as error:
                note(error)
    def interrupted(signum, frame):
        raise TimeoutError('independent executor TERM')
    signal.signal(signal.SIGTERM, interrupted)
    try:
        require(Path(args.approval) == approval_path, 'exact approval path required')
        role_control = STAGE / args.role / 'control'
        require(Path(args.admission) == role_control / (revision+'-' + args.step + '-admission-001.json') and Path(args.output) == role_control / run_id, 'exact role output/admission required')
        approval = read_json(approval_path, approval_sha)
        admission = read_json(args.admission)
        slots=DIAGNOSE_SLOTS if formal else ([('builder','diagnose-contract',40),('reviewer','diagnose-contract',40)] if diagnostic else RESUME_SLOTS)
        require([(row['role'], row['step'], row['seconds']) for row in approval['stage_allowlist']] == slots, 'approved fixed slot scope mismatch')
        require(admission['schema'] == revision+'-execution-admission-v1' and admission['role'] == args.role and admission['step'] == args.step and admission['run_id'] == run_id and admission['approved'] is True and admission['static_review'] == 'PASS' and admission['approval_sha256'] == approval_sha, 'independent admission missing')
        if diagnostic:
            require(approval['schema']=='r033-approved-preparation-v1' and approval['preparation_storage_bytes']==33554432
                    and approval['formal_sampling_authorized'] is False and approval['formal_analysis_authorized'] is False
                    and approval['failure_stops_all'] is True and approval['no_retry'] is True
                    and approval['design_sha256']=='51be17fc1210946acd2be8655d3f1a1110aa9d2ac2eea7e705cf97452aae56fe','R033 preparation authority only')
        if formal:
            require(approval['schema']=='r034-approved-diagnosis-v1' and approval['per_role_storage_bytes']==536870912
                    and approval['formal_sampling_authorized'] is True and approval['formal_analysis_authorized'] is True
                    and approval['failure_stops_all'] is True and approval['no_retry'] is True
                    and approval['design_sha256']=='ce9abce1ce4154f4c49f5802656380684dacaf998a2d56cd72e05a3e49efeed8','R034 authority')
        slot_index = slots.index((args.role, args.step, maximum))
        if formal and args.step=='diagnose-offline':
            require(type(admission['base_negative']) is bool,'exact negative gate bool')
        if formal and args.step=='diagnose-offline' and args.role=='builder' and admission.get('base_negative') is True:
            slot_index=slots.index(('builder','diagnose-observe',45))
        if slot_index:
            previous_role, previous_step, _ = slots[slot_index - 1]
            predecessor_path = STAGE / previous_role / 'control' / (revision+'-' + previous_step + '-001') / 'result.json'
            require(admission['predecessor_path'] == str(predecessor_path), 'exact predecessor path required')
            predecessor = read_json(predecessor_path, admission['predecessor_sha256'])
            require(predecessor['status'] == 'valid' and predecessor['cleanup_complete'] is True, 'predecessor not complete valid')
            require(admission['predecessor_time_wall_sha256'] == sha(STAGE / previous_role / 'control' / (revision+'-' + previous_step + '-001.wall')) and admission['predecessor_exit_code'] == 0, 'actual predecessor wall/exit binding missing')
            if formal and args.step in ('diagnose-observe','diagnose-offline'):
                base=read_json(STAGE/'builder/control/r034-diagnose-base-driver-001/sample.json',admission['base_sample_sha256'])
                require(base['status']=='valid' and base['cleanup']['complete'] and base['first_error'] is None,'base completeness')
                positive=bool(base['result']['validated']['slow_records'])
                require((positive if args.step=='diagnose-observe' else positive != admission['base_negative']),'base conditional gate')
        elif not diagnostic and not formal:
            require(admission['r031_actual_verified'] is True, 'first resume needs independent R031 actual PASS')
        elif formal:
            require(admission['r033_actual_verified'] is True,'R033 preparation actual PASS binding required')
        inputs = read_json(admission['inputs_path'], admission['inputs_sha256'])
        preparation_source_bytes=inputs['preparation_source_bytes'] if diagnostic or formal else 0
        require(type(preparation_source_bytes) is int and preparation_source_bytes>=0,'source material charge')
        table = read_json(admission['commands_path'], admission['commands_sha256'])
        seal = read_json(admission['seal_path'], admission['seal_sha256'])
        for manifest in (inputs, seal):
            paths = set()
            for entry in manifest['files']:
                require(entry['path'] not in paths, 'duplicate closure item')
                paths.add(entry['path'])
                require(sha(entry['path']) == entry['sha256'], 'execution closure drift')
                closure.append(entry)
        required = {str(Path(__file__).absolute()), str(REPO / 'benchmark/revalidation/test_revalidate.py'),
                    str(REPO / 'benchmark/revalidation/revalidate.py')}
        if diagnostic or formal:
            required.update(str(REPO/'benchmark/tail-localization/diagnose_d'/name) for name in ('collect.py','analyze.py','test_diagnose.py'))
        else:
            required.add(str(approval_path))
        require(required <= {item['path'] for item in closure}, 'actual execution Python/approval closure missing')
        reviewer_drivers=None
        if formal and args.role=='reviewer':
            reviewer_drivers=inputs['reviewer_drivers']
            for mode in ('entry','offline'):
                driver=role_control/('r034-reviewer-'+mode+'.py')
                require(reviewer_drivers[mode]['path']==str(driver) and sha(driver)==reviewer_drivers[mode]['sha256']
                        and str(driver) in {item['path'] for item in closure},'independent Reviewer driver binding')
        for tool in inputs['system_tools']:
            require(tool['path'] in ('/usr/bin/time', '/usr/bin/timeout', '/usr/bin/python3') and sha(tool['path']) == tool['sha256'], 'trusted system tool drift')
        require({tool['path'] for tool in inputs['system_tools']} == {'/usr/bin/time', '/usr/bin/timeout', '/usr/bin/python3'}, 'trusted tools incomplete')
        row = next(row for row in table['rows'] if (row['role'], row['step']) == (args.role, args.step))
        require(hashlib.sha256(canonical(row)).hexdigest() == admission['row_sha256'], 'exact canonical row mismatch')
        require(row['argv'][11:] == sys.argv and row['cwd'] == str(REPO), 'actual entry argv mismatch')
        require(row['argv'][:11] == ['/usr/bin/time', '-f', '%e', '-o', str(role_control / (run_id + '.wall')),
                '/usr/bin/timeout', '--signal=TERM', '--kill-after=2s', str(maximum - 2) + 's', '/usr/bin/python3', '-B'], 'fixed trusted outer row mismatch')
        expected_environment = {'PATH': '/usr/bin:/bin', 'PYTHONDONTWRITEBYTECODE': '1',
            'NO_PROXY': '127.0.0.1,localhost', 'LD_LIBRARY_PATH': str(REPO / '.cache/v0.5-s4/tools/root/usr/lib/x86_64-linux-gnu'),
            'TMPDIR': str(STAGE / args.role / 'tmp'), 'TMP': str(STAGE / args.role / 'tmp'),
            'TEMP': str(STAGE / args.role / 'tmp'), 'XDG_CACHE_HOME': str(STAGE / args.role / 'cache')}
        require(row['environment'] == expected_environment, 'complete fixed environment rejected')
        external_logs = [role_control / (run_id + '.' + suffix) for suffix in ('wall', 'stdout', 'stderr')]
        for name, value in row['environment'].items():
            require(os.environ.get(name) == value, 'actual role environment mismatch')
        me = identity(os.getpid())
        parent = identity(os.getppid())
        require(me is not None and parent is not None and me['pgid'] == parent['pid'] == parent['pgid'], 'actual trusted timeout group required')
        scope_ns = namespace(os.getpid())
        trusted_parent = parent
        require(namespace(parent['pid']) == scope_ns, 'outer namespace mismatch')
        time_parent = identity(parent['ppid'])
        require(time_parent is not None and namespace(time_parent['pid']) == scope_ns, 'actual time ancestor identity missing')
        tool_hashes = {tool['path']: tool['sha256'] for tool in inputs['system_tools']}
        require(sha('/proc/' + str(parent['pid']) + '/exe') == tool_hashes['/usr/bin/timeout']
                and sha('/proc/' + str(time_parent['pid']) + '/exe') == tool_hashes['/usr/bin/time'], 'actual time/timeout executable mismatch')
        # Timeout starttime is a real earlier anchor, translated from BOOTTIME to MONOTONIC.
        ticks = os.sysconf('SC_CLK_TCK')
        timeout_start = parent['starttime'] * 1000000000 // ticks
        started = min(ENTRY_NS, time.monotonic_ns() - (time.clock_gettime_ns(time.CLOCK_BOOTTIME) - timeout_start))
        work_at, cleanup_at = started + (maximum - 10) * 1000000000, started + (maximum - 4) * 1000000000
        require(time.monotonic_ns() < work_at and not driver_output.exists(), 'work budget/driver once already consumed')
        real_parents(Path(args.output))
        Path(args.output).mkdir(exist_ok=False)
        output = Path(args.output)
        require(save('intent.json', {'row': row, 'row_sha256': admission['row_sha256'], 'approval_sha256': approval_sha,
                    'admission': admission, 'executor': me, 'timeout': parent, 'time': time_parent, 'namespace': scope_ns,
                    'started_ns': started, 'work_deadline_ns': work_at, 'cleanup_deadline_ns': cleanup_at,
                    'time_scope': 'GNU time start through executor subtree termination; its final wall publication is observation layer'}), 'intent save unavailable')
        shared = ['--shared-work-deadline-ns', str(work_at), '--shared-cleanup-deadline-ns', str(cleanup_at)]
        if diagnostic:
            argv=['/usr/bin/python3','-B',str(REPO/'benchmark/tail-localization/diagnose_d/collect.py'),
                  '--role',args.role,'--output',str(driver_output),*shared]
        elif formal:
            if args.step=='diagnose-offline':
                offline_source=reviewer_drivers['offline']['path'] if reviewer_drivers else str(REPO/'benchmark/tail-localization/diagnose_d/analyze.py')
                argv=['/usr/bin/python3','-B',offline_source,'--role',args.role,'--output',str(driver_output),*shared,
                      '--sample',str(STAGE/'builder/control/r034-diagnose-base-driver-001')]
                if not admission['base_negative']:argv+=['--sample',str(STAGE/'builder/control/r034-diagnose-observe-driver-001')]
            else:
                collection_source=reviewer_drivers['entry']['path'] if reviewer_drivers else str(REPO/'benchmark/tail-localization/diagnose_d/collect.py')
                argv=['/usr/bin/python3','-B',collection_source,'--role',args.role,'--mode',args.step.removeprefix('diagnose-'),'--output',str(driver_output),*shared]
        elif args.step == 'contract':
            argv = ['/usr/bin/python3', '-B', str(REPO / 'benchmark/revalidation/test_revalidate.py'), '--output', str(driver_output), *shared]
        else:
            action, label = args.step.split('-')
            argv = ['/usr/bin/python3', '-B', str(REPO / 'benchmark/revalidation/revalidate.py'), '--role', args.role, *shared, action, '--label', label]
            if action == 'smoke':
                argv += ['--output', str(driver_output)]
        for name in ('driver.stdout', 'driver.stderr'):
            streams.append((output / name).open('xb'))
        require(save('launch.json', {'argv': argv, 'environment': row['environment']}), 'launch save unavailable')
        spawn_attempted = True
        process = subprocess.Popen(argv, cwd=REPO, env=row['environment'], stdout=streams[0], stderr=streams[1])
        driver_identity = identity(process.pid)
        require(driver_identity is not None and driver_identity['ppid'] == os.getpid() and namespace(process.pid) == scope_ns, 'spawned driver registration unknown')
        owned[(driver_identity['pid'], driver_identity['starttime'])] = driver_identity
        while process.poll() is None:
            require(time.monotonic_ns() < cleanup_at - 1000000000, 'driver exceeded cleanup handoff')
            register()
            material_bytes()
            time.sleep(.1)
        code = process.wait(timeout=.01)
        require(code == 0, 'fixed revalidation driver nonzero')
        if diagnostic:
            result=read_json(driver_output/'contract-result.json')
            require(result['schema']=='r033-contract-v1' and result['status']=='valid'
                    and result['fixture']['tests']==10 and result['fixture']['failures']==0 and result['fixture']['errors']==0
                    and result['cleanup']['complete'] is True and result['first_error'] is None and result['errors']==[]
                    and result['formal_sampling_authorized'] is False and result['actual_D_started'] is False,'R033 offline contract invalid')
        elif formal:
            if args.step=='diagnose-entry':
                result=read_json(driver_output/'contract-result.json')
                require(result['status']=='valid' and result['fixture']['tests']>0 and result['fixture']['failures']==result['fixture']['errors']==0
                        and result['cleanup']['complete'] and result['first_error'] is None and result['errors']==[],'entry result invalid')
            elif args.step=='diagnose-offline':
                result=read_json(driver_output/'offline-result.json')
                require(result['schema']=='r034-offline-v1' and result['status']=='valid' and result['first_error'] is None
                        and len(result['samples'])==(1 if admission['base_negative'] else 2),'offline result invalid')
                require(result['samples'][0]['sample_sha256']==admission['base_sample_sha256'],'offline base identity')
                if not admission['base_negative']:
                    require(result['samples'][1]['sample_sha256']==admission['observe_sample_sha256'],'offline observed identity')
            else:
                result=read_json(driver_output/'sample.json')
                require(result['status']=='valid' and result['mode']==args.step.removeprefix('diagnose-')
                        and result['cleanup']['complete'] and result['first_error'] is None and result['errors']==[],'formal sample invalid')
                for artifact in result['catalog']:
                    item=Path(artifact['path'])
                    require(item.name==artifact['path'] and sha(driver_output/item)==artifact['sha256'] and (driver_output/item).stat().st_size==artifact['bytes'],'formal catalog binding')
        elif args.step == 'contract':
            result = read_json(driver_output / 'contract-result.json')
            require(result['status'] == 'valid' and result['tests'] == 9 and result['failures'] == 0 and result['errors'] == 0, 'original nine contract assertions not complete')
        else:
            result = read_json(role_control / (args.step + '.result.json'))
            require(result['status'] == 'valid' and result['cleanup']['complete'] is True and not result['cleanup']['remaining']
                    and all(not child['forced'] and not child['errors'] for child in result['cleanup']['children']), 'inner Scope result/cleanup invalid')
            payload = result['result']
            if args.step.startswith('build-'):
                require(payload['manifest'] == str(driver_output / 'manifest.json'), 'exact produced manifest path required')
                manifest = read_json(payload['manifest'], payload['manifest_sha256'])
                label = args.step[-1]
                expected = {'C': ('942f72cd9cea58e097025c3b9dd660f4132a1ffb', 'b407f052c7a974ae4fff4976c8275fb5905cf036'),
                            'D': ('69424e6ab057bba2950c018e34c5695a4dc74f22', '826e20166caa334c95a1c6fdc957a128d5aee568')}[label]
                require((manifest['commit'], manifest['tree']) == expected and manifest['role'] == args.role and sha(manifest['binary']) == manifest['binary_sha256'], 'actual build identity invalid')
            else:
                require(payload['sample'] == str(driver_output / 'sample/sample.json'), 'exact smoke sample path required')
                sample = read_json(payload['sample'], payload['sample_sha256'])
                require(sample['status'] == 'valid' and sample['cleanup']['reaped'] and not sample['cleanup']['forced'], 'actual smoke not valid/reaped')
    except BaseException as error:
        note(error)
    finally:
        try:
            signal.signal(signal.SIGTERM, signal.SIG_IGN)
        except BaseException as error:
            note(error)
        if process is not None and driver_identity is None:
            try:
                value = identity(process.pid)
                require(value is not None and value['ppid'] == os.getpid() and namespace(value['pid']) == scope_ns, 'created driver ownership unknown')
                driver_identity = value
                owned[(value['pid'], value['starttime'])] = value
            except BaseException as error:
                note(error)
        try:
            register()
        except BaseException as error:
            note(error)
        rows = observe()
        if any(row['classification'] != 'absent' for row in rows):
            stop_owned(signal.SIGTERM)
            if process is not None:
                try:
                    process.wait(timeout=min(.3, max(.001, (cleanup_at - time.monotonic_ns()) / 1e9)))
                except BaseException as error:
                    note(error)
            stop_owned(signal.SIGKILL)
        if process is not None:
            try:
                process.wait(timeout=max(.001, (cleanup_at - time.monotonic_ns()) / 1e9))
            except BaseException as error:
                note(error)
        rows = observe()
        for stream in streams:
            try:
                stream.close()
            except BaseException as error:
                note(error)
        total = None
        try:
            total = material_bytes() if output is not None else None
            require(total is not None and total <= 2147483648, 'final material unknown/limit')
        except BaseException as error:
            note(error)
        for entry in closure:
            try:
                require(time.monotonic_ns() < cleanup_at and sha(entry['path']) == entry['sha256'], 'final immutable closure drift/deadline')
            except BaseException as error:
                note(error)
        complete = process is not None and driver_identity is not None and not failures and all(row['classification'] == 'absent' for row in rows)
        save('outer-evidence.json', {'namespace': scope_ns, 'owned': list(owned.values()), 'observations': observations,
             'first_error': first, 'errors': failures, 'spawn_attempted': spawn_attempted, 'cleanup_complete': complete,
             'material_bytes_before_final_records': total, 'started_ns': started, 'checked_ns': time.monotonic_ns()})
        valid = first is None and complete and evidence_ok and time.monotonic_ns() < cleanup_at
        saved = save('result.json', {'status': 'valid' if valid else 'unknown', 'cleanup_complete': complete,
             'first_error': first, 'errors': failures, 'ended_before_durable_result_ns': time.monotonic_ns(),
             'started_ns': started, 'time_wall_is_authoritative_and_external_exit_required': True,
             'conservative_seconds_if_unknown': maximum, 'failure_stops_all': not valid, 'role': args.role, 'step': args.step,
             'inner_result': result if 'result' in locals() else None})
        try:
            signal.signal(signal.SIGTERM, before_term)
        except BaseException as error:
            note(error)
            valid = False
        require(valid and saved and time.monotonic_ns() < cleanup_at, 'independent execution unknown; stop')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--role', choices=('builder', 'reviewer'), required=True)
    parser.add_argument('--step', choices=('contract', 'build-C', 'build-D', 'smoke-C', 'smoke-D', 'diagnose-contract','diagnose-entry','diagnose-base','diagnose-observe','diagnose-offline'), required=True)
    parser.add_argument('--approval', required=True)
    parser.add_argument('--admission', required=True)
    parser.add_argument('--output', required=True)
    execute(parser.parse_args())


if __name__ == '__main__':
    main()
