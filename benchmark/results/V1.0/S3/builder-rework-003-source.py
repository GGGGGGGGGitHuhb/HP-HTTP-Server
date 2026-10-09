#!/usr/bin/env python3
"""Approved S3/R003 single Builder batch. No product or permanent tool changes."""
import ast, copy, ctypes, errno, hashlib, json, math, os, pathlib, re, resource
import select, shutil, signal, socket, statistics, subprocess, sys, time

REPO=pathlib.Path.cwd().resolve()
ROOT=(REPO/'.cache/v1.0-s3/builder/rework-003').resolve()
SOURCE=(ROOT/'source').resolve()
GENERATED=[(SOURCE/name).resolve() for name in ('build','build-release','build-asan','.validation')]
CANDIDATE='20bd03142b4c828a7e939c30b501812da2bd5440'
PATCH_SHA='6ad80910acf90acc1c76ad3fefb4034864d521ae282f18500c30731acfd52b83'
WRK=REPO/'.cache/v0.5-s4/tools/root/usr/bin/wrk'
LIB=REPO/'.cache/v0.5-s4/tools/root/usr/lib/x86_64-linux-gnu'
LUA=REPO/'benchmark/matrix/Summary.lua'
ENV=dict(os.environ)
for key in ('CXXFLAGS','CPPFLAGS','LDFLAGS','HP_S3_TEST_TMP_ROOT','HP_MATRIX_TEST_TMP_ROOT','HP_ANALYSIS_TEST_TMP_ROOT','ASAN_OPTIONS','UBSAN_OPTIONS'):
    ENV.pop(key,None)
ENV.update(TMPDIR=str(ROOT/'tmp'),TMP=str(ROOT/'tmp'),TEMP=str(ROOT/'tmp'),XDG_CACHE_HOME=str(ROOT/'cache'),PYTHONDONTWRITEBYTECODE='1',NO_PROXY='127.0.0.1,localhost',no_proxy='127.0.0.1,localhost')
PUBLIC_OUTPUTS=[REPO/'benchmark/results/V1.0/S3'/name for name in ('builder-rework-003.json','builder-rework-003.md','builder-rework-003-source.py')]+[REPO/'docs/builder/reports/V1.0/S3-report-003.md',REPO/'documentation/RELEASE-CHECK.md']
TRACE=ROOT/'logs/governance.jsonl'
STATE={}; OWNED=[]; ACTIVE=False; NEXT_TICK=0.; ACTIVE_BEGIN=0.
LAST_START=None; LAST_COMPLETE=None; SOFT_CLEANUP=False
CANON_ROOT=str(ROOT); CANON_SOURCE=str(SOURCE)
CANON_BUILD=[str(path) for path in GENERATED[:3]]

def require(condition,message):
    if not condition: raise ValueError(message)

def within(path,parent):
    return path==parent or path.startswith(parent+os.sep)

def emit(kind,**fields):
    if not ACTIVE: return
    with TRACE.open('a') as handle:
        handle.write(json.dumps(dict(kind=kind,monotonic=time.monotonic(),**fields),ensure_ascii=False,allow_nan=False)+'\n')

def save(path,data):
    if ACTIVE: guard()
    path.write_text(json.dumps(data,indent=2,ensure_ascii=False,allow_nan=False)+'\n')
    if ACTIVE: guard()

def sha(path):
    digest=hashlib.sha256()
    with path.open('rb') as handle:
        while chunk:=handle.read(65536):
            digest.update(chunk)
            if ACTIVE: guard()
    return digest.hexdigest()

def identity(pid):
    try:
        text=pathlib.Path(f'/proc/{pid}/stat').read_text()
        return text[text.rfind(')')+2:].split()[19]
    except FileNotFoundError: return None

def classify(path):
    captured=path in STATE.get('captures',{})
    label='registered-capture' if captured else 'ordinary-file'
    for build in CANON_BUILD:
        if not within(path,build): continue
        parts=os.path.relpath(path,build).split(os.sep)
        if parts[0]=='Testing' or '/'.join(parts) in ('CMakeFiles/CMakeConfigureLog.yaml','CMakeFiles/CMakeOutput.log','CMakeFiles/CMakeError.log'):
            captured=True; label='CMake/CTest-capture'
        if parts[0]=='test-tmp' and os.path.splitext(path)[1] in ('.stdout','.stderr','.log'):
            sub=parts[1:]
            fixture=(len(sub)==2 and sub[0].startswith('synthetic-') and sub[1]=='oversize.stdout') or (len(sub)==3 and sub[0].startswith('synthetic-') and tuple(sub[1:]) in [('sample-01','server.stderr'),('sample-02','measurement.stdout')])
            captured=not fixture; label='frozen-test-input' if fixture else 'frozen-test-process-capture'
    smoke=str(GENERATED[3]/'smoke')
    if within(path,smoke) and os.path.basename(path)=='server.log':
        captured=True; label='HTTP-smoke-process-capture'
    return captured,label

def raw_scan():
    total=logs=files=0; categories={}
    # os.walk never follows directory links; lstat excludes file links, as before.
    for directory,_,names in os.walk(CANON_ROOT,followlinks=False):
        for name in names:
            path=os.path.join(directory,name)
            try: st=os.stat(path,follow_symlinks=False)
            except FileNotFoundError: continue
            if not __import__('stat').S_ISREG(st.st_mode): continue
            charge=max(st.st_size,st.st_blocks*512); total+=charge; files+=1
            capture,label=classify(path)
            if capture: logs+=charge
            if label!='ordinary-file': categories[os.path.relpath(path,CANON_ROOT)]=label
    # Public artifacts have an explicitly registered producer and exact paths.
    # They enter the same role charge before that producer starts; no directory sweep.
    for path in STATE.get('publication_outputs',[]):
        try: st=os.stat(path,follow_symlinks=False)
        except FileNotFoundError: continue
        require(__import__('stat').S_ISREG(st.st_mode),'public output is not regular file')
        total+=max(st.st_size,st.st_blocks*512); files+=1
        categories[path]='declared-public-artifact'
    return total,logs,categories,files

def limit_errors(total,logs,total_limit=4*1024**3,log_limit=512*1024**2):
    return (['role total byte limit'] if total>total_limit else [])+(['real capture byte limit'] if logs>log_limit else [])

def take_scan(reason):
    global LAST_START,LAST_COMPLETE,NEXT_TICK
    start=time.monotonic(); total,logs,categories,files=raw_scan(); complete=time.monotonic()
    errors=[]; duration=complete-start
    start_gap=start-(LAST_START if LAST_START is not None else ACTIVE_BEGIN)
    complete_gap=complete-(LAST_COMPLETE if LAST_COMPLETE is not None else ACTIVE_BEGIN)
    if max(duration,start_gap,complete_gap)>1.: errors.append('active scan interval/duration exceeded 1s')
    errors.extend(limit_errors(total,logs))
    stats=STATE.setdefault('scan_stats',dict(count=0,total_duration_s=0.,max_duration_s=0.,max_start_gap_s=0.,max_complete_gap_s=0.,max_boundary_gap_s=0.))
    stats['count']+=1; stats['total_duration_s']+=duration
    for key,val in [('max_duration_s',duration),('max_start_gap_s',start_gap),('max_complete_gap_s',complete_gap)]: stats[key]=max(stats[key],val)
    STATE['peak_total_bytes']=max(total,STATE.get('peak_total_bytes',0)); STATE['peak_log_bytes']=max(logs,STATE.get('peak_log_bytes',0))
    STATE['last_snapshot']=dict(total_bytes=total,capture_bytes=logs,files=files,scan_start=start,scan_complete=complete)
    STATE.setdefault('dynamic_classifications',{}).update(categories)
    LAST_START=start; LAST_COMPLETE=complete
    # Next point lies on the original 0.25s grid; missed points are never replayed.
    NEXT_TICK=ACTIVE_BEGIN+(math.floor((complete-ACTIVE_BEGIN)/.25)+1)*.25
    emit('scan',reason=reason,start=start,complete=complete,duration_s=duration,start_gap_s=start_gap,complete_gap_s=complete_gap,total_bytes=total,capture_bytes=logs,files=files,errors=errors,next_fixed_tick=NEXT_TICK)
    if errors: STATE.setdefault('governance_errors',[]).extend(errors)

def guard(deadline=None,force=False,soft=False):
    if not ACTIVE: return
    now=time.monotonic(); errors=[]
    if now>=STATE['started_monotonic']+(1800 if soft or SOFT_CLEANUP else 1790): errors.append('global deadline')
    phase_deadline=STATE.get('phase_deadline')
    if phase_deadline is not None and now>=phase_deadline: errors.append('persistent phase deadline')
    if deadline is not None and now>=deadline: errors.append('phase/operation deadline')
    if force or now>=NEXT_TICK: take_scan('boundary' if force else 'fixed-tick')
    # A scan itself may cross a deadline; recheck the fresh clock before release.
    now=time.monotonic()
    if now>=STATE['started_monotonic']+(1800 if soft or SOFT_CLEANUP else 1790): errors.append('global deadline after scan')
    if phase_deadline is not None and now>=phase_deadline: errors.append('persistent phase deadline after scan')
    if deadline is not None and now>=deadline: errors.append('operation deadline after scan')
    if errors:
        STATE.setdefault('governance_errors',[]).extend(errors); emit('governance-failure',errors=errors)
    if STATE.get('governance_errors') and not (soft or SOFT_CLEANUP):
        raise ValueError(STATE['governance_errors'][0])

def begin_active(phase):
    global ACTIVE,ACTIVE_BEGIN,LAST_START,LAST_COMPLETE,NEXT_TICK
    ACTIVE=True; ACTIVE_BEGIN=time.monotonic(); LAST_START=LAST_COMPLETE=None; NEXT_TICK=ACTIVE_BEGIN
    STATE['captures'][str(TRACE)]='driver-governance-trace'
    STATE.setdefault('active_segments',[]).append(dict(phase=phase,start=ACTIVE_BEGIN,driver_is_producer=True))
    guard(force=True)
    emit('active-start',phase=phase,static_handoff_since=STATE.get('quiescent_since'))

def finish_active():
    global ACTIVE
    require(not OWNED,'handoff still has owned producers')
    guard(force=True,soft=True)
    end=time.monotonic(); endpoint=max(end-LAST_START,end-LAST_COMPLETE)
    STATE['scan_stats']['max_boundary_gap_s']=max(endpoint,STATE['scan_stats']['max_boundary_gap_s'])
    if endpoint>1.: STATE.setdefault('governance_errors',[]).append('active end boundary exceeded 1s')
    STATE['active_segments'][-1].update(closing_marker=end,all_external_owned_waited=True,capture_handles_closed=True,closing_marker_gap_s=endpoint)
    STATE['handoff']=dict(all_external_producers_waited=True,driver_active_through_final_metadata=True,boundary_marker=end)
    emit('active-end-intent',end=end,endpoint_gap_s=endpoint,external_producer_count=0)
    # Trace and captured child files are now closed. Only the known state ledger
    # is written after this real final full scan; its exact allocation delta is
    # reconciled to a stable fixed point, with the same logical/allocated rule.
    take_scan_without_trace()
    path=ROOT/'manifest/state.json'
    previous=path.stat(); baseline_charge=max(previous.st_size,previous.st_blocks*512)
    base_total=STATE['final_total_bytes']; final_capture=STATE['final_capture_bytes']
    if STATE.get('governance_errors'):
        STATE['status']='failed'; STATE.setdefault('failure',STATE['governance_errors'][0])
    STATE['elapsed_s']=time.monotonic()-STATE['started_monotonic']
    STATE['final_metadata_reconciliation']=dict(producer='driver',path=str(path),full_scan_complete=LAST_COMPLETE,only_file_written_after_full_scan=True,rule='sum(max(logical,allocated)); exact last ledger-file delta')
    STATE['handoff']['terminal_actual_scan_evidence']='final tool stdout; no role-file write follows this final full scan'
    stable=False
    for _ in range(8):
        path.write_text(json.dumps(STATE,indent=2,ensure_ascii=False,allow_nan=False)+'\n')
        current=path.stat(); actual=base_total+max(current.st_size,current.st_blocks*512)-baseline_charge
        if actual==STATE['final_total_bytes']: stable=True; break
        STATE['final_total_bytes']=actual; STATE['peak_total_bytes']=max(actual,STATE['peak_total_bytes'])
    require(stable,'final metadata accounting did not converge')
    require(time.monotonic()-LAST_COMPLETE<=1.,'final metadata producer interval exceeded 1s')
    require(not limit_errors(STATE['final_total_bytes'],final_capture),'final accounting limit')
    # Last full scan covers the last state-file write. Its result is returned in
    # the tool's terminal output, rather than writing another unscanned ledger.
    final_start=time.monotonic(); total,logs,_,files=raw_scan(); final_complete=time.monotonic()
    terminal=dict(start=final_start,complete=final_complete,start_gap_s=final_start-LAST_START,complete_gap_s=final_complete-LAST_COMPLETE,duration_s=final_complete-final_start,total_bytes=total,capture_bytes=logs,files=files)
    require(max(terminal['start_gap_s'],terminal['complete_gap_s'],terminal['duration_s'])<=1.,'terminal actual scan interval exceeded 1s')
    require((total,logs)==(STATE['final_total_bytes'],STATE['final_capture_bytes']),'last-write/final-scan accounting mismatch')
    STATE['terminal_actual_scan']=terminal
    STATE['terminal_active_end']=time.monotonic()
    require(STATE['terminal_active_end']-final_complete<=1.,'terminal active endpoint exceeded 1s')
    ACTIVE=False

def take_scan_without_trace():
    global LAST_START,LAST_COMPLETE
    start=time.monotonic(); total,logs,_,files=raw_scan(); complete=time.monotonic()
    duration=complete-start; start_gap=start-LAST_START; complete_gap=complete-LAST_COMPLETE
    stats=STATE['scan_stats']; stats['count']+=1; stats['total_duration_s']+=duration
    for key,value in [('max_duration_s',duration),('max_start_gap_s',start_gap),('max_complete_gap_s',complete_gap)]: stats[key]=max(stats[key],value)
    if max(duration,start_gap,complete_gap)>1.: STATE.setdefault('governance_errors',[]).append('final active scan exceeded 1s')
    STATE['peak_total_bytes']=max(total,STATE['peak_total_bytes']); STATE['peak_log_bytes']=max(logs,STATE['peak_log_bytes'])
    STATE.update(final_total_bytes=total,final_capture_bytes=logs,last_final_scan=dict(start=start,complete=complete,files=files))
    STATE.setdefault('governance_errors',[]).extend(limit_errors(total,logs)); LAST_START=start; LAST_COMPLETE=complete

def descendants(pid):
    result=[]
    for entry in pathlib.Path('/proc').glob('[0-9]*/stat'):
        try:
            fields=entry.read_text().rsplit(')',1)[1].split()
            if int(fields[1])==pid:
                child=int(entry.parent.name); result.append((child,identity(child))); result.extend(descendants(child))
        except (FileNotFoundError,ProcessLookupError): pass
    return result

def launch(argv,label,cwd=SOURCE,extra=None):
    guard(force=True)
    prefix=ROOT/'logs'/label; out=prefix.with_suffix('.stdout'); err=prefix.with_suffix('.stderr')
    require(within(str(out.resolve()),CANON_ROOT) and within(str(err.resolve()),CANON_ROOT),'capture path escaped role root')
    handles=[out.open('wb'),err.open('wb')]
    STATE['captures'].update({str(out.resolve()):label,str(err.resolve()):label})
    env=dict(ENV); env.update(extra or {})
    process=subprocess.Popen([str(x) for x in argv],cwd=cwd,env=env,stdout=handles[0],stderr=handles[1],start_new_session=True)
    entry=dict(argv=[str(x) for x in argv],cwd=str(cwd),env={key:env[key] for key in ('TMPDIR','TMP','TEMP','XDG_CACHE_HOME','PYTHONDONTWRITEBYTECODE','NO_PROXY','no_proxy')},pid=process.pid,starttime=identity(process.pid),stdout=str(out),stderr=str(err),children={})
    entry['env'].update(extra or {}); STATE.setdefault('commands',[]).append(entry)
    item=(process,handles,entry); OWNED.append(item); emit('process-start',pid=process.pid,starttime=entry['starttime'],label=label)
    guard(); return item

def pause(seconds,deadline=None,soft=False):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        guard(deadline,soft=soft)
        select.select([],[],[],min(.05,max(0.,end-time.monotonic())))
    guard(deadline,soft=soft)

def reap(item,requested_signal=None):
    process,handles,entry=item; forced=False
    if requested_signal is not None and process.poll() is None:
        require(identity(process.pid)==entry['starttime'],'PID identity changed')
        process.send_signal(requested_signal); emit('process-signal',pid=process.pid,signal=requested_signal)
    end=time.monotonic()+5
    while process.poll() is None and time.monotonic()<end:
        guard(soft=True); pause(.05,soft=True)
    if process.poll() is None:
        require(identity(process.pid)==entry['starttime'],'PID identity changed before KILL')
        process.kill(); forced=True
    end=time.monotonic()+2
    while process.poll() is None and time.monotonic()<end: pause(.05,soft=True)
    require(process.poll() is not None,'owned parent unreaped after KILL')
    process.wait() # poll has already reaped; this call cannot block.
    survivors=[(int(pid),start) for pid,start in entry['children'].items() if start and identity(int(pid))==start]
    for pid,start in survivors:
        if identity(pid)==start:
            try: os.kill(pid,signal.SIGTERM)
            except ProcessLookupError: pass
    end=time.monotonic()+1
    for pid,start in survivors:
        while identity(pid)==start and time.monotonic()<end:
            try:
                if os.waitpid(pid,os.WNOHANG)[0]: break
            except ChildProcessError: pass
            pause(.05,soft=True)
        if identity(pid)==start:
            os.kill(pid,signal.SIGKILL); forced=True
        end_kill=time.monotonic()+2
        while identity(pid)==start and time.monotonic()<end_kill:
            try:
                if os.waitpid(pid,os.WNOHANG)[0]: break
            except ChildProcessError: pass
            pause(.05,soft=True)
        require(identity(pid) is None,'owned descendant unreaped')
    for handle in handles: handle.close()
    entry.update(returncode=process.returncode,forced=forced,reaped=True,descendant_survivors=len(survivors)); OWNED.remove(item)
    emit('process-reaped',pid=process.pid,returncode=process.returncode,forced=forced)
    guard(force=True,soft=True)
    require(not forced and not survivors,'forced/orphan cleanup invalidates operation')
    return entry

def wait_command(item,deadline,acceptable=(0,)):
    process,_,entry=item; began=time.monotonic()
    try:
        while process.poll() is None:
            for pid,start in descendants(process.pid): entry['children'][str(pid)]=start
            guard(deadline); pause(.05,deadline)
        result=reap(item); result['elapsed_s']=time.monotonic()-began
        guard(deadline,force=True)
        require(result['returncode'] in acceptable,'nonzero exit '+str(result['argv'])+': '+str(result['returncode']))
        return result
    finally:
        if item in OWNED: reap(item,signal.SIGTERM)

def command(argv,label,deadline,cwd=SOURCE,extra=None,acceptable=(0,)):
    return wait_command(launch(argv,label,cwd,extra),deadline,acceptable)

def verify_source():
    baseline=json.loads((ROOT/'manifest/source-files.json').read_text()); observed={}
    for directory,_,names in os.walk(SOURCE):
        if ACTIVE: guard()
        if any(within(directory,str(root)) for root in GENERATED): continue
        for name in names:
            path=pathlib.Path(directory)/name
            if path.is_file(): observed[str(path.relative_to(SOURCE))]=sha(path)
            if ACTIVE: guard()
    require(baseline==observed,'candidate original file drift/unregistered generated output')

def ready_socket(peer,read,deadline):
    while True:
        guard()
        remaining=deadline-time.monotonic(); require(remaining>0,'socket operation timeout')
        reads,writes,_=select.select([peer] if read else [],[] if read else [peer],[],min(.1,remaining))
        guard(); require(time.monotonic()<deadline,'socket operation timeout')
        if reads or writes: return

def connect_peer(port):
    peer=socket.socket(); peer.setblocking(False); deadline=time.monotonic()+2
    code=peer.connect_ex(('127.0.0.1',port))
    try:
        if code not in (0,errno.EISCONN):
            require(code in (errno.EINPROGRESS,errno.EWOULDBLOCK,errno.EALREADY),'connect failed '+str(code))
            ready_socket(peer,False,deadline); require(peer.getsockopt(socket.SOL_SOCKET,socket.SO_ERROR)==0,'connect SO_ERROR')
        guard(); require(time.monotonic()<deadline,'connect operation timeout'); return peer
    except Exception:
        peer.close(); raise

def send_all(peer,data,tag):
    offset=0; deadline=time.monotonic()+2
    while offset<len(data):
        guard(); require(time.monotonic()<deadline,'send operation timeout')
        try:
            count=peer.send(data[offset:]); require(count>0,'send EOF'); offset+=count
            emit('http-send',tag=tag,offset=offset,total=len(data),operation_deadline=deadline)
        except BlockingIOError: ready_socket(peer,False,deadline)
    guard(); require(time.monotonic()<deadline,'send operation timeout')

def receive(peer,size,tag):
    deadline=time.monotonic()+2
    while True:
        guard(); require(time.monotonic()<deadline,'receive operation timeout')
        try:
            data=peer.recv(size); emit('http-recv',tag=tag,bytes=len(data),operation_deadline=deadline,eof=not data); return data
        except BlockingIOError: ready_socket(peer,True,deadline)

def readiness(item,deadline):
    end=min(deadline,time.monotonic()+5)
    while item[0].poll() is None:
        guard(end)
        match=re.search(r'listening on port (\d+)\.',pathlib.Path(item[2]['stdout']).read_text())
        if match:
            port=int(match[1]); require(owned_listener(item,port),'listener ownership'); return port
        pause(.02,end)
    raise ValueError('server exited before readiness')

def audit(port,payload,deadline,label):
    observations=[]; peer=None; index=-1; header_bytes=body_bytes=0
    try:
        emit('audit-connect-begin',label=label); peer=connect_peer(port)
        for index in range(3):
            guard(deadline); header_bytes=body_bytes=0
            emit('audit-request-begin',label=label,index=index,body_expected=len(payload))
            send_all(peer,b'GET /payload.bin HTTP/1.1\r\nHost: loopback\r\nConnection: keep-alive\r\n\r\n',f'{label}/{index}')
            received=b''
            while b'\r\n\r\n' not in received:
                part=receive(peer,65536,f'{label}/{index}/header'); require(part,'header EOF'); received+=part; header_bytes=len(received)
            header,body=received.split(b'\r\n\r\n',1); header_bytes=len(header); body_bytes=len(body)
            require(header.startswith(b'HTTP/1.1 200 '),'audit status')
            headers=dict(line.split(b':',1) for line in header.split(b'\r\n')[1:])
            require(int(headers[b'Content-Length'])==len(payload),'Content-Length')
            require(headers[b'Connection'].strip()==b'keep-alive','keepalive')
            emit('audit-header-complete',label=label,index=index,header_bytes=header_bytes,initial_body_bytes=body_bytes)
            while len(body)<len(payload):
                part=receive(peer,65536,f'{label}/{index}/body'); require(part,'body EOF'); body+=part; body_bytes=len(body)
                emit('audit-body-progress',label=label,index=index,body_bytes=body_bytes,expected=len(payload)); guard(deadline)
            require(body==payload,'body hash or tail mismatch')
            tail_end=time.monotonic()+.02
            while time.monotonic()<tail_end:
                guard(deadline)
                readable=select.select([peer],[],[],min(.1,max(0.,tail_end-time.monotonic())))[0]
                require(not readable,'unsolicited tail bytes or premature close')
            observations.append(dict(index=index,status=200,content_length=len(body),sha256=hashlib.sha256(body).hexdigest(),same_socket_index=index))
            emit('audit-request-complete',label=label,observation=observations[-1])
    except Exception as exc:
        emit('audit-failure',label=label,index=index,header_bytes=header_bytes,body_bytes=body_bytes,error=str(exc),completed_prefix=observations)
        raise
    finally:
        if peer is not None: peer.close()
        emit('audit-peer-closed',label=label,completed_requests=len(observations))
    return observations

def sample(name,config,warm,duration,deadline):
    manifest=json.loads((ROOT/'manifest/release.json').read_text()); binary=pathlib.Path(manifest['binary'])
    require(binary==GENERATED[1]/'hp_http_server' and sha(binary)==manifest['binary_sha256'] and manifest['candidate']==CANDIDATE and manifest['patch_sha256']==PATCH_SHA,'Release input manifest')
    payload=b'0123456789abcdef'*(config['bytes']//16)
    directory=GENERATED[3]/name; require(not directory.exists(),'sample directory already exists'); directory.mkdir()
    (directory/'payload.bin').write_bytes(payload); guard()
    row=dict(name=name,config=config,status='pending',started_monotonic=time.monotonic())
    STATE['samples'].append(row); save(ROOT/'manifest/state.json',STATE); emit('sample-pending',name=name)
    server=None; original_failure=False
    try:
        server=launch([binary,'--port','0','--root',directory,'--threads',str(config['workers']),'--idle-timeout-ms','30000','--keep-alive-timeout-ms','15000','--shutdown-timeout-ms','5000','--metrics-on-exit'],name+'-server')
        port=readiness(server,deadline); row['port']=port
        row['server_identity']=dict(pid=server[2]['pid'],starttime=server[2]['starttime'],listener_owned=True)
        row['stage']='pre_audit'; save(ROOT/'manifest/state.json',STATE)
        row['pre_audit']=audit(port,payload,deadline,name+'/pre')
        for phase,seconds in [('warmup',warm),('measurement',duration)]:
            row['stage']=phase; save(ROOT/'manifest/state.json',STATE)
            argv=[WRK,'-t',str(config['threads']),'-c',str(config['connections']),'--timeout','2s','--latency','-d',str(seconds)+'s','-s',LUA,f'http://127.0.0.1:{port}/payload.bin']
            result=command(argv,name+'-'+phase,deadline,extra=dict(LD_LIBRARY_PATH=str(LIB),HP_MATRIX_MODE='keepalive',HP_MATRIX_CONNECTIONS=str(config['connections'])))
            text=pathlib.Path(result['stdout']).read_text(); row[phase]=parse_summary(text,result['returncode'],len(payload),config['connections'])
            row[phase+'_raw_stdout']=text
        row['stage']='post_audit'; save(ROOT/'manifest/state.json',STATE)
        row['post_audit']=audit(port,payload,deadline,name+'/post')
        row['stage']='cleanup'; row['cleanup']=reap(server,signal.SIGTERM); require(row['cleanup']['returncode']==0,'server normal exit')
        row['metrics']=metrics(pathlib.Path(server[2]['stdout']).read_text())
        probe=socket.socket(); probe.setblocking(False)
        try:
            code=probe.connect_ex(('127.0.0.1',port))
            if code in (errno.EINPROGRESS,errno.EWOULDBLOCK):
                ready_socket(probe,False,time.monotonic()+.2); code=probe.getsockopt(socket.SOL_SOCKET,socket.SO_ERROR)
            require(code!=0,'owned port still listening after reap')
        finally: probe.close()
        guard(deadline); row['status']='valid'
    except Exception as exc:
        original_failure=True
        row.update(status='invalid',failure=str(exc),failure_stage=row.get('stage','startup'))
        emit('sample-invalid',name=name,failure=str(exc),stage=row.get('stage'))
        raise
    finally:
        try:
            if server is not None and server in OWNED:
                try: row['cleanup']=reap(server,signal.SIGTERM)
                except Exception as exc:
                    row.update(status='invalid',cleanup_failure=str(exc)); row['cleanup']=server[2]
            if server is not None:
                text=pathlib.Path(server[2]['stdout']).read_text()
                if 'HP_METRICS_BEGIN' in text:
                    try: row['metrics']=metrics(text)
                    except Exception as exc: row['metrics_failure']=str(exc)
        finally:
            shutil.rmtree(directory); guard(soft=True)
            row['finished_monotonic']=time.monotonic()
            STATE['not_run']=[item for item in STATE['planned_formal'] if item not in {r['name'] for r in STATE['samples']}]
            # Soft only controls cleanup routing; the latched failure remains.
            before=globals()['SOFT_CLEANUP']; globals()['SOFT_CLEANUP']=True
            try:
                save(ROOT/'performance'/f'{name}.json',row); save(ROOT/'manifest/state.json',STATE)
                emit('sample-settled',name=name,status=row['status'],not_run=STATE['not_run'])
            finally: globals()['SOFT_CLEANUP']=before
        if not original_failure: require(row['status']=='valid','sample cleanup/metrics did not settle valid')

def synthetic(deadline):
    results=[]
    data=dict(schema=2,requests=100,duration_us=1000000,bytes=102400,errors=dict(connect=0,read=0,write=0,status=0,timeout=0),latency_us=dict(mean=2,p50=1,p95=3,p99=4,max=5),latency_distribution='wrk_corrected',population_status='not_collected',corrected_population=None,nonzero_bins=None,correction_interval_us=320000)
    text='MATRIX_SUMMARY '+json.dumps(data); parse_summary(text,0,1024,32)
    variants=[]
    for key in ('requests','duration_us','bytes','errors','latency_us','corrected_population'):
        bad=copy.deepcopy(data); del bad[key]; variants.append((key,'MATRIX_SUMMARY '+json.dumps(bad),0))
    for name,change in [('count',('requests',0)),('error',None),('latency',None)]:
        bad=copy.deepcopy(data)
        if name=='count': bad['requests']=0
        elif name=='error': bad['errors']['timeout']=1
        else: bad['latency_us']['p99']=.5
        variants.append((name,'MATRIX_SUMMARY '+json.dumps(bad),0))
    variants.extend([('missing','',0),('duplicate',text+'\n'+text,0),('nonzero',text,1)])
    for name,encoded,code in variants:
        try: parse_summary(encoded,code,1024,32)
        except (ValueError,KeyError): results.append(name)
        else: raise ValueError('accepted summary negative '+name)
    require(all(not path.exists() for path in GENERATED),'generated initial tree precreated')
    # Exact original scan is injected onto this current tree, not the old tree.
    namespace=dict(pathlib=pathlib,ROOT=ROOT,SOURCE=SOURCE,GENERATED=GENERATED,STATE=STATE)
    original=(ROOT/'manifest/frozen-original-scan.py').read_text(); exec(compile(original,'frozen-scan','exec'),namespace)
    fixture=GENERATED[0]/'test-tmp/synthetic-r003'
    fixture.mkdir(parents=True); sparse=fixture/'oversize.stdout'
    with sparse.open('wb') as handle: handle.truncate(2*1024**3+1)
    other=ROOT/'tmp/same-name/build/test-tmp/synthetic-r003'; other.mkdir(parents=True)
    (other/'oversize.stdout').write_bytes(b'not a capture')
    real=ROOT/'logs/fixture-capture.stdout'; real.write_bytes(b'capture'); STATE['captures'][str(real.resolve())]='synthetic-real-capture'
    (ROOT/'tmp/symlink-file').symlink_to(real)
    (ROOT/'tmp/symlink-dir').symlink_to(GENERATED[0],target_is_directory=True)
    guard(force=True)
    old_started=time.monotonic(); old_total,old_logs,old_categories=namespace['scan'](); old_finished=time.monotonic()
    new_total,new_logs,new_categories,_=raw_scan(); new_finished=time.monotonic()
    require((old_total,old_logs)==(new_total,new_logs),'original/current true-tree scan mismatch')
    require(all(new_categories.get(key)==value for key,value in old_categories.items()),'frozen fixture category drift')
    require(classify(str(sparse))[0] is False and classify(str(other/'oversize.stdout'))[0] is False and classify(str(real))[0] is True,'path classification negative')
    require(not within(str(GENERATED[0])+'-neighbor/file',str(GENERATED[0])),'directory prefix boundary')
    equivalence=dict(total_bytes=old_total,capture_bytes=old_logs,original_scan_s=old_finished-old_started,new_scan_s=new_finished-old_finished,original_scan_sha256=sha(ROOT/'manifest/frozen-original-scan.py'),same_actual_tree=True)
    guard(force=True)
    (ROOT/'tmp/symlink-file').unlink(); (ROOT/'tmp/symlink-dir').unlink(); shutil.rmtree(GENERATED[0])
    require(not GENERATED[0].exists(),'build initial absence not restored')
    require(limit_errors(10,10,total_limit=9,log_limit=9)==['role total byte limit','real capture byte limit'],'quota predicate')
    results.extend(['actual-tree-old-scan-equivalence','symlink-file/dir','canonical-same-name-non-target','directory-boundary','sparse-input-log-classification','initial-build-absence-restored','quota-predicate'])
    # Exercise the real I/O wrapper with fragmented reads and a single absolute deadline.
    left,right=socket.socketpair(); left.setblocking(False); right.setblocking(False)
    try:
        class ScriptedSender:
            def __init__(self): self.calls=[]; self.index=0
            def fileno(self): return left.fileno()
            def send(self,data):
                self.calls.append(data); self.index+=1
                if self.index==2: raise BlockingIOError()
                return min(2,len(data))
        scripted=ScriptedSender(); send_all(scripted,b'abcdef','synthetic-short-write')
        require(scripted.calls==[b'abcdef',b'cdef',b'cdef',b'ef'],'partial-send suffix reset')
        send_all(left,b'abcdef','synthetic-send-suffix')
        require(receive(right,2,'synthetic-short-read')==b'ab' and receive(right,4,'synthetic-short-read')==b'cdef','short receive semantics')
        began=time.monotonic(); endpoint=began+.22
        try: ready_socket(left,True,endpoint)
        except ValueError as exc: require('timeout' in str(exc),'wrong deadline branch')
        else: raise ValueError('socket empty-read deadline did not stop')
        require(time.monotonic()-began<.5,'absolute socket deadline reset/overshoot')
    finally: left.close(); right.close()
    results.extend(['partial-send-suffix-through-EAGAIN','fragmented-recv','socket-EAGAIN/select-slices','absolute-operation-deadline-no-reset'])
    child=launch([sys.executable,'-c','import signal,time; signal.signal(signal.SIGTERM,lambda *_:None); print("ready",flush=True); time.sleep(.4)'],'synthetic-term-wait',cwd=ROOT/'tmp')
    try:
        while 'ready' not in pathlib.Path(child[2]['stdout']).read_text():
            require(child[0].poll() is None,'TERM fixture died'); pause(.01,deadline)
        entry=reap(child,signal.SIGTERM); require(entry['returncode']==0 and not entry['forced'],'TERM poll governance')
    finally:
        if child in OWNED: reap(child,signal.SIGTERM)
    results.append('TERM-short-poll-finally-reap')
    pending=[dict(name='r1-M2',status='pending')]
    require('r1-M2' not in [name for name in ['r1-M2','r1-M3'] if name not in {r['name'] for r in pending}],'pending counted as NotRun')
    results.append('pending-started-versus-NotRun')
    # Fixed grid evaluation is a pure contract negative, not an ungoverned sleep.
    anchor=10.; complete=10.81; tick=anchor+(math.floor((complete-anchor)/.25)+1)*.25
    require(tick==11. and max(.05,.99,.99)<=1 and max(.05,1.01,.99)>1,'fixed tick/interval predicate')
    results.extend(['fixed-grid-no-backlog','scan-start/complete-boundary-limit-predicate'])
    save(ROOT/'manifest/synthetic.json',dict(passed=results,equivalence=equivalence,owned_left=len(OWNED)))

def prepare():
    require(not (ROOT/'manifest/state.json').exists(),'single new batch already started')
    require(all(not path.exists() for path in GENERATED),'generated initial state exists')
    require(sha(ROOT/'manifest/candidate.patch')==PATCH_SHA,'frozen four-file patch drift')
    baseline={str(path.relative_to(SOURCE)):sha(path) for path in SOURCE.rglob('*') if path.is_file()}
    approved=json.loads((REPO/'.cache/v1.0-s3/builder/rework-001/manifest/candidate-files.json').read_text())
    require(baseline==approved and len(baseline)==581,'581 candidate table drift')
    save(ROOT/'manifest/source-files.json',baseline)
    require(sha(WRK)=='b10e53769443c2bf3be2cdedec8ef6571aa5bfd1494796b247f3f9296e3af71d','wrk SHA')
    require(sha(LUA)=='0706c8defa1a759eda90185ca5c9f02d755bb3c5a7248367d5f0c3618111340e','Lua SHA')
    tools={str(path):sha(path) for path in [WRK,LUA]+[path for path in LIB.iterdir() if path.is_file()]}
    save(ROOT/'manifest/inputs.json',dict(candidate=CANDIDATE,tree=subprocess.check_output(['git','rev-parse',CANDIDATE+'^{tree}'],cwd=REPO,text=True).strip(),archive_sha256=sha(ROOT/'source.tar'),patch_sha256=PATCH_SHA,candidate_files_sha256=sha(ROOT/'manifest/source-files.json'),original_file_count=581,tools=tools,generated_trees=[str(path) for path in GENERATED],initial_generated_exist=[path.exists() for path in GENERATED],public_output_paths=[str(path) for path in PUBLIC_OUTPUTS],public_output_producer='owned package-results Python subprocess; exact-path role charge before launch',scan_tick_s=.25,maximum_scan_interval_s=1,socket_operation_timeout_s=2,select_slice_s=.1,tail_window_s=.02,term_normal_wait_s=5))
    free=shutil.disk_usage(ROOT).free; mem=int(re.search(r'MemAvailable:\s+(\d+)',pathlib.Path('/proc/meminfo').read_text())[1])*1024
    require(free>=4*1024**3 and mem>=1024**3 and resource.getrlimit(resource.RLIMIT_NOFILE)[0]>=256,'resource static admission')
    save(ROOT/'manifest/static-admission.json',dict(free_disk_bytes=free,mem_available_bytes=mem,nofile=resource.getrlimit(resource.RLIMIT_NOFILE),driver_sha256=sha(pathlib.Path(__file__)),budget_s=1800,old_builder_elapsed_s=621.2861129630182,cleanup_reserved_s=10,route=str(REPO)))
    print('STATIC_READY',flush=True)

def main():
    global STATE,SOFT_CLEANUP
    entered_main=time.monotonic()
    phase=sys.argv[1]
    if phase=='prepare': prepare(); return
    ctypes.CDLL(None).prctl(36,1,0,0,0); resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    path=ROOT/'manifest/state.json'
    if phase=='governance':
        require(not path.exists(),'restarting batch forbidden')
        STATE=dict(started_monotonic=entered_main,started_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),old_builder_elapsed_s=621.2861129630182,status='running',phases=[],commands=[],captures={},samples=[],planned_formal=['r1-M2','r1-M3','r1-M6','r2-M6','r2-M3','r2-M2','r3-M3','r3-M2','r3-M6'])
    else:
        STATE=json.loads(path.read_text()); require(STATE['status']=='running' or (phase=='package' and STATE['status']=='passed' and not STATE.get('packaged')),'failed/settled batch cannot continue')
    try:
        stage_start=time.monotonic(); STATE['current_phase']=phase
        cap=600 if phase.startswith('build-') else 600-STATE.get('functional_elapsed_s',0) if phase.startswith('tests-') else 360 if phase=='performance' else 1790
        STATE['phase_deadline']=min(stage_start+cap,STATE['started_monotonic']+1790)
        begin_active(phase); save(path,STATE)
        if phase=='governance':
            free=shutil.disk_usage(ROOT).free; mem=int(re.search(r'MemAvailable:\s+(\d+)',pathlib.Path('/proc/meminfo').read_text())[1])*1024; nofile=resource.getrlimit(resource.RLIMIT_NOFILE)
            STATE['dynamic_resource_admission']=dict(free_disk_bytes=free,mem_available_bytes=mem,nofile=nofile,monotonic=time.monotonic())
            save(path,STATE); require(free>=4*1024**3 and mem>=1024**3 and nofile[0]>=256,'resource dynamic admission')
            synthetic(min(time.monotonic()+60,STATE['started_monotonic']+1790))
            checker_probe(min(time.monotonic()+60,STATE['started_monotonic']+1790))
            for tool,args in [('cmake',['--version']),('g++',['--version']),('python3',['--version']),('curl',['--version']),('bash',['--version'])]: command([tool]+args,'version-'+tool,time.monotonic()+10,cwd=ROOT)
            command([WRK,'--version'],'version-wrk',time.monotonic()+10,cwd=ROOT,extra=dict(LD_LIBRARY_PATH=str(LIB)),acceptable=(0,1))
            command(['python3',REPO/'scripts/format_cpp.py','--check','--files-from',ROOT/'manifest/format-files.txt'],'format-four-file-check',time.monotonic()+20,cwd=REPO)
            STATE['functional_elapsed_s']=STATE.get('functional_elapsed_s',0)+time.monotonic()-stage_start
            require(STATE['functional_elapsed_s']<=600,'functional total deadline')
        elif phase=='package':
            helper=ROOT/'tmp/package.py'; require(helper.is_file(),'missing result packaging input')
            STATE['publication_outputs']=[str(path) for path in PUBLIC_OUTPUTS]
            STATE['publication_producer']='owned package-results Python subprocess'
            guard(force=True)
            STATE['package_helper_sha256']=sha(helper)
            command(['python3',helper],'package-results',STATE['phase_deadline'],cwd=REPO)
            STATE['packaged']=True
        elif phase.startswith('build-'): build(phase[6:])
        elif phase.startswith('tests-'):
            tests(phase[6:])
            if phase=='tests-debug':
                began=time.monotonic(); cli(min(began+600-STATE.get('functional_elapsed_s',0),STATE['started_monotonic']+1790)); STATE['functional_elapsed_s']+=time.monotonic()-began
        elif phase=='performance':
            deadline=min(time.monotonic()+360,STATE['started_monotonic']+1790)
            configs=dict(M2=dict(bytes=1024,workers=2,threads=2,connections=32),M3=dict(bytes=1024,workers=4,threads=4,connections=128),M6=dict(bytes=1048576,workers=2,threads=2,connections=32))
            sample('smoke-M2',configs['M2'],1,1,deadline)
            for name in STATE['planned_formal']: sample(name,configs[name.split('-')[1]],2,10,deadline)
            STATE['aggregates']={}
            for scene in configs:
                selected=[r['measurement'] for r in STATE['samples'] if r['name'].startswith('r') and r['name'].endswith(scene)]
                STATE['aggregates'][scene]={key:dict(median=statistics.median(values),min=min(values),max=max(values)) for key,values in [('qps',[r['qps'] for r in selected]),('received_mib_s',[r['received_mib_s'] for r in selected]),('corrected_p99_us',[r['latency_us']['p99'] for r in selected])]}
            STATE['status']='passed'
        else: raise ValueError('unknown phase')
        verify_source(); guard(force=True); STATE['phases'].append(dict(phase=phase,status='passed',completed_monotonic=time.monotonic()))
    except Exception as exc:
        STATE.update(status='failed',failure=str(exc)); STATE['phases'].append(dict(phase=phase,status='failed',failure=str(exc))); emit('first-failure',phase=phase,error=str(exc))
    finally:
        SOFT_CLEANUP=True
        for item in list(OWNED):
            try: reap(item,signal.SIGTERM)
            except Exception as exc: STATE.setdefault('cleanup_failures',[]).append(str(exc))
        STATE['not_run']=[name for name in STATE['planned_formal'] if name not in {r['name'] for r in STATE['samples']}]
        STATE.update(elapsed_s=time.monotonic()-STATE['started_monotonic'],owned_left=len(OWNED))
        save(path,STATE); finish_active()
        print(json.dumps({key:STATE.get(key) for key in ('current_phase','status','failure','elapsed_s','peak_total_bytes','peak_log_bytes','final_total_bytes','final_capture_bytes','owned_left','scan_stats','terminal_actual_scan','terminal_active_end')},ensure_ascii=False),flush=True)
    sys.exit(1 if STATE['status']=='failed' else 0)



def parse_summary(text, code, size, connections):
    require(code==0,'wrk exit')
    lines=[line[15:] for line in text.splitlines() if line.startswith('MATRIX_SUMMARY ')]
    require(len(lines)==1,'summary count')
    def pairs(items):
        result={}
        for key,value in items:
            require(key not in result,'duplicate JSON key'); result[key]=value
        return result
    data=json.loads(lines[0],object_pairs_hook=pairs)
    require(data['schema']==2,'schema')
    for key in ('requests','duration_us','bytes'):
        val=data[key]; require(type(val) in (int,float) and math.isfinite(val) and val>0 and int(val)==val,'count '+key)
    require(set(data['errors'])=={'connect','read','write','status','timeout'},'error fields')
    require(all(type(v) in (int,float) and v==0 for v in data['errors'].values()),'errors')
    lat=data['latency_us']; require(set(lat)=={'mean','p50','p95','p99','max'},'latency fields')
    require(all(type(v) in (int,float) and math.isfinite(v) and v>=0 for v in lat.values()),'finite latency')
    require(lat['p50']<=lat['p95']<=lat['p99']<=lat['max'] and lat['mean']<=lat['max'],'latency ordering')
    require(data['latency_distribution']=='wrk_corrected' and data['population_status']=='not_collected' and data['corrected_population'] is None and data['nonzero_bins'] is None,'population')
    require(abs(data['correction_interval_us']-data['duration_us']*connections/data['requests'])<=1e-5,'interval')
    require(data['bytes']>=data['requests']*size,'body count')
    data.update(qps=data['requests']/(data['duration_us']/1e6),received_mib_s=data['bytes']/(data['duration_us']/1e6)/1048576)
    return data

def checker_probe(deadline):
    target=ROOT/'tmp/checker-probe'
    source=ROOT/'tmp/checker-probe.cpp'
    command(['/usr/bin/g++','-std=c++20','-O3','-DNDEBUG','-I',SOURCE/'tests',source,'-Wl,--wrap=malloc','-Wl,--wrap=_Znwm','-o',target],'checker-compile',deadline,cwd=ROOT/'tmp')
    true_case=command([target],'checker-true',deadline,cwd=ROOT/'tmp')
    require(pathlib.Path(true_case['stdout']).stat().st_size==pathlib.Path(true_case['stderr']).stat().st_size==0,'checker successful path emitted output')
    false_case=command([target,'false'],'checker-false',deadline,cwd=ROOT/'tmp',acceptable=(-signal.SIGABRT,))
    error=pathlib.Path(false_case['stderr']).read_text()
    require('Test condition failed at ' in error and 'checker-probe.cpp:' in error,'checker missing caller file/line')
    save(ROOT/'manifest/checker-probe.json',dict(release_flags=['-O3','-DNDEBUG'],true_returncode=true_case['returncode'],true_side_effect_evaluations=1,true_allocations=0,true_stdout_bytes=0,true_stderr_bytes=0,false_returncode=false_case['returncode'],false_stderr=error,caller_source_sha256=sha(source),core_files_disabled_for_owned_processes=True))

def build(kind):
    directory=GENERATED[{'debug':0,'release':1,'asan':2}[kind]]
    require(not directory.exists(),'build already exists')
    deadline=min(time.monotonic()+600,STATE['started_monotonic']+1790)
    argv=['cmake','-S',SOURCE,'-B',directory,'-DCMAKE_BUILD_TYPE='+('Release' if kind=='release' else 'Debug'),'-DBUILD_TESTING=ON','-DCMAKE_CXX_COMPILER=/usr/bin/g++','-DCMAKE_EXPORT_COMPILE_COMMANDS=ON']
    if kind=='release': argv+=['-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG']
    if kind=='asan': argv+=['-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer','-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined']
    command(argv,kind+'-configure',deadline)
    command(['cmake','--build',directory,'-j2'],kind+'-build',deadline)
    registration=command(['ctest','--test-dir',directory,'--show-only=json-v1'],kind+'-registration',deadline)
    tests=json.loads(pathlib.Path(registration['stdout']).read_text())['tests']
    require(len(tests)==14,'CTest registration count')
    binary=directory/'hp_http_server'
    deps=command(['ldd',binary],kind+'-ldd',deadline)
    libraries={}
    for token in pathlib.Path(deps['stdout']).read_text().split():
        if token.startswith('/') and pathlib.Path(token).is_file(): libraries[token]=sha(pathlib.Path(token))
    manifest=dict(candidate=CANDIDATE,patch_sha256=sha(ROOT/'manifest/candidate.patch'),build=kind,binary=str(binary),binary_sha256=sha(binary),libraries=libraries,tests=tests,compile_commands_sha256=sha(directory/'compile_commands.json'),cmake_cache_sha256=sha(directory/'CMakeCache.txt'))
    save(ROOT/'manifest'/f'{kind}.json',manifest)
    verify_source()

def tests(kind):
    began=time.monotonic(); deadline=min(began+600-STATE.get('functional_elapsed_s',0),STATE['started_monotonic']+1790)
    directory=GENERATED[{'debug':0,'release':1,'asan':2}[kind]]
    try:
        argv=['ctest','--test-dir',directory,'--output-on-failure','-j1']
        extra={}
        if kind=='asan':
            argv+=['-R','^(async_logger_batch_tests|r6_callbacks_tests|tcp_nodelay_tests|server_metrics_tests|http_observability_tests)$']
            extra=dict(ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        command(argv,kind+'-ctest',deadline,extra=extra)
        if kind!='asan':
            (SOURCE/'.validation/smoke').mkdir(parents=True,exist_ok=True)
            for threads in (0,2):
                command(['bash',SOURCE/'tests/http_smoke_test.sh',directory/'hp_http_server'],f'{kind}-smoke-{threads}',deadline,extra=dict(HP_S3_TEST_TMP_ROOT=str(SOURCE/'.validation/smoke'),HP_HTTP_TEST_THREADS=str(threads)))
        verify_source()
    finally: STATE['functional_elapsed_s']=STATE.get('functional_elapsed_s',0)+time.monotonic()-began

def owned_listener(obj,port):
    p,_,entry=obj
    require(identity(p.pid)==entry['starttime'],'listener PID changed')
    inodes=set()
    for fd in pathlib.Path(f'/proc/{p.pid}/fd').iterdir():
        try: target=os.readlink(fd)
        except FileNotFoundError: continue
        if target.startswith('socket:['): inodes.add(target[8:-1])
    return any(int(parts[1].split(':')[1],16)==port and parts[3]=='0A' and parts[9] in inodes for parts in (line.split() for line in pathlib.Path('/proc/net/tcp').read_text().splitlines()[1:]))

def metrics(text):
    require(text.count('HP_METRICS_BEGIN')==text.count('HP_METRICS_END')==1,'metrics markers')
    values=dict(line.split() for line in text.split('HP_METRICS_BEGIN\n')[1].split('HP_METRICS_END')[0].splitlines())
    values={key:int(val) for key,val in values.items()}
    require(values['connections_active']==values['logger_pending']==0,'final active resources')
    require(values['requests_started_total']==values['responses_completed_total']+values['requests_aborted_total']==values['latency_count'],'final accounting')
    return values

def cli(deadline):
    binary=SOURCE/'build/hp_http_server'
    command([binary,'--help'],'readme-cli-help',deadline)
    for name,args in [('missing-all',[]),('missing-root',['--port','0']),('missing-port',['--root',SOURCE/'www'])]:
        command([binary]+args,'cli-'+name,deadline,acceptable=(2,))
    obj=launch([binary,'--port','8080','--root','./www','--threads','2'],'readme-server')
    try:
        try: port=readiness(obj,deadline)
        except ValueError:
            # Approved fallback applies only to actual port occupation.
            out=pathlib.Path(obj[2]['stderr']).read_text(); require('Address already in use' in out,'README startup failure')
            reap(obj); STATE['readme_port_fallback']='8080 occupied; used supported port0'
            obj=launch([binary,'--port','0','--root','./www','--threads','2'],'readme-port0-server'); port=readiness(obj,deadline)
        for label,expected,args in [('index',200,[]),('missing',404,[]),('post',405,['-X','POST'])]:
            url=f'http://127.0.0.1:{port}/'+('missing.txt' if label=='missing' else '')
            result=command(['curl','--silent','--show-error','--max-time','3','-o',ROOT/'tmp'/f'{label}.body','-w','%{http_code}']+args+[url],'readme-'+label,deadline)
            require(pathlib.Path(result['stdout']).read_text()==str(expected),'README response '+label)
            if label=='index': require((ROOT/'tmp/index.body').read_bytes()==(SOURCE/'www/index.html').read_bytes(),'README www body')
        result=reap(obj,signal.SIGINT); require(result['returncode']==0,'SIGINT normal exit')
    finally:
        if obj in OWNED: reap(obj,signal.SIGTERM)


if __name__=='__main__': main()
