#!/usr/bin/env python3
"""Approved R002 one-shot evidence capture; no rerun/acceptance authority."""
import ast, datetime, hashlib, json, os, pathlib, re, resource, select, shutil, signal, socket, subprocess, sys, time
REPO=pathlib.Path.cwd().resolve()
OLD=REPO/'.cache/v1.0-s3/builder/rework-001'
ROOT=REPO/'.cache/v1.0-s3/reviewer/diagnostic-001'
MAN=ROOT/'manifest'
CAPTURES={str((ROOT/'logs'/x).resolve()):x for x in ('trace.jsonl','server.stdout','server.stderr')}

def require(ok,msg):
    if not ok: raise ValueError(msg)
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def save(p,data): p.write_text(json.dumps(data,indent=2,ensure_ascii=False)+'\n')
def inventory(root):
    files={}; total=0
    for p in sorted(root.rglob('*')):
        if p.is_symlink(): files[str(p.relative_to(root))]={'symlink':os.readlink(p)}
        elif p.is_file():
            st=p.stat();charge=max(st.st_size,st.st_blocks*512);total+=charge
            files[str(p.relative_to(root))]={'sha256':sha(p),'logical':st.st_size,'charge':charge}
    return {'files':files,'charge':total}
def newscan():
    total=logs=0
    for p in ROOT.rglob('*'):
        if p.is_file() and not p.is_symlink():
            st=p.stat();c=max(st.st_size,st.st_blocks*512);total+=c
            if str(p.resolve()) in CAPTURES: logs+=c
    return total,logs
def loadscan():
    source=(OLD/'tmp/validate.py').read_text();node=next(n for n in ast.parse(source).body if isinstance(n,ast.FunctionDef) and n.name=='scan')
    source_text=ast.get_source_segment(source,node)
    frozen=json.loads((OLD/'manifest/state.json').read_text())
    ns={'ROOT':OLD,'SOURCE':OLD/'source','GENERATED':[OLD/'source'/x for x in ('build','build-release','build-asan','.validation')],'STATE':{'captures':frozen['captures']}}
    exec(compile(ast.Module(body=[node],type_ignores=[]),'<exact-frozen-scan>','exec'),ns)
    return ns['scan'],source_text,ns

def prepare():
    require(not (ROOT/'payload').exists(),'payload must be absent')
    require(not OLD.is_relative_to(ROOT) and not ROOT.is_relative_to(OLD),'canonical roots intersect')
    release=json.loads((OLD/'manifest/release.json').read_text());binary=pathlib.Path(release['binary'])
    require(sha(binary)==release['binary_sha256'],'binary identity')
    for p,h in release['libraries'].items():require(sha(pathlib.Path(p))==h,'runtime library identity')
    require(sha(OLD/'manifest/candidate.patch')==release['patch_sha256'],'patch identity')
    scan,text,ns=loadscan() # compile only; no scan/admission in preparation
    (MAN/'exact-scan.txt').write_text(text+'\n')
    before=inventory(OLD);save(MAN/'old-before.json',before)
    save(MAN/'prepared.json',{'release':release,'old_root':str(OLD),'new_root':str(ROOT),'payload_initially_absent':True,'scan_sha256':hashlib.sha256(text.encode()).hexdigest(),'producer_source_sha256':sha(OLD/'tmp/validate.py'),'captures':ns['STATE']['captures'],'generated':[str(x) for x in ns['GENERATED']],'old_snapshot_files':len(before['files']),'old_snapshot_charge':before['charge'],'snapshot_limitation':'post-cleanup snapshot; original payload removed and final logs landed; not failure-instant file set','source_sha256':sha(pathlib.Path(__file__))})
    print(json.dumps({'prepared':True,'old_files':len(before['files']),'old_charge':before['charge'],'binary_sha256':release['binary_sha256'],'scan_sha256':hashlib.sha256(text.encode()).hexdigest()}))

def identity(pid):
    try:
        s=pathlib.Path(f'/proc/{pid}/stat').read_text();return s[s.rfind(')')+2:].split()[19]
    except FileNotFoundError:return None

def listener(pid,start,port):
    if identity(pid)!=start:return False
    inodes=set()
    for fd in pathlib.Path(f'/proc/{pid}/fd').iterdir():
        try:t=os.readlink(fd)
        except FileNotFoundError:continue
        if t.startswith('socket:['):inodes.add(t[8:-1])
    return any(int(a[1].split(':')[1],16)==port and a[3]=='0A' and a[9] in inodes for a in (x.split() for x in pathlib.Path('/proc/net/tcp').read_text().splitlines()[1:]))

def execute():
    require(not (MAN/'state.json').exists(),'one-shot state already exists')
    prepared=json.loads((MAN/'prepared.json').read_text());require(sha(pathlib.Path(__file__))==prepared['source_sha256'],'prepared source changed')
    scan,text,ns=loadscan();require(hashlib.sha256(text.encode()).hexdigest()==prepared['scan_sha256'],'scan changed')
    require(ns['STATE']['captures']==prepared['captures'],'captures changed')
    release=prepared['release'];binary=pathlib.Path(release['binary']);require(sha(binary)==release['binary_sha256'],'binary changed')
    start=time.monotonic();utc=datetime.datetime.now(datetime.timezone.utc)
    state={'status':'running','started_monotonic':start,'started_utc':utc.isoformat(),'diagnostic_expiry_monotonic':start+180,'role_expiry_monotonic':start+1800,'role_expiry_utc':(utc+datetime.timedelta(seconds=1800)).isoformat(),'peak_total_bytes':0,'peak_log_bytes':0,'completed_requests':[],'owned_left':0,'original_role_clock_continues':True}
    save(MAN/'state.json',state)
    print(json.dumps({'clock_started_utc':state['started_utc'],'role_expiry_utc':state['role_expiry_utc'],'started_monotonic':start}),flush=True)
    trace=(ROOT/'logs/trace.jsonl').open('a',buffering=1);writes=[];guards=[];server=None;handles=[];peer=None;port=None;server_id=None;current=None;error=None;forced=False
    def event(kind,**kw):
        t=time.monotonic();row={'kind':kind,'mono':t,'elapsed_s':t-start,**kw};trace.write(json.dumps(row,separators=(',',':'))+'\n');trace.flush();writes.append({'event':kind,'start':t,'end':time.monotonic()})
    def guard(deadline,position):
        begin=time.monotonic();event('guard_begin',position=position);s=time.monotonic();oldtotal,oldlogs,categories=scan();e=time.monotonic();n0=time.monotonic();nt,nl=newscan();n1=time.monotonic();end=time.monotonic()
        row={'position':position,'begin':begin,'scan_begin':s,'scan_end':e,'new_scan_begin':n0,'new_scan_end':n1,'end':end,'old_total':oldtotal,'old_logs':oldlogs,'new_total':nt,'new_logs':nl,'total':oldtotal+nt,'logs':oldlogs+nl};guards.append(row)
        state['peak_total_bytes']=max(state['peak_total_bytes'],row['total']);state['peak_log_bytes']=max(state['peak_log_bytes'],row['logs']);state['old_classifications']=categories
        event('guard_end',**row)
        require(end<min(start+170,deadline),'diagnostic deadline (10s cleanup reserve)');require(row['total']<=4*1024**3,'combined total limit');require(row['logs']<=512*1024**2,'combined capture limit')
    def snapshot(reason):
        t=time.monotonic();alive=server is not None and server.poll() is None and identity(server.pid)==server_id
        event('failure_snapshot',reason=reason,request_index=current,server_alive=alive,pid=server.pid if server else None,starttime=server_id,listener_owned=listener(server.pid,server_id,port) if alive and port else None,stderr_bytes=(ROOT/'logs/server.stderr').stat().st_size if (ROOT/'logs/server.stderr').exists() else None,snapshot_start=t,snapshot_end=time.monotonic())
    try:
        event('clock_start',role_expiry_utc=state['role_expiry_utc'])
        require(shutil.disk_usage(ROOT).free>=4*1024**3,'free disk');mem=int(re.search(r'MemAvailable:\s+(\d+)',pathlib.Path('/proc/meminfo').read_text()).group(1))*1024;require(mem>=1024**3,'available memory');require(resource.getrlimit(resource.RLIMIT_NOFILE)[0]>=256,'nofile')
        require(not (ROOT/'payload').exists(),'payload initial state changed');require(not OLD.is_relative_to(ROOT) and not ROOT.is_relative_to(OLD),'root disjoint')
        # Local path/capture governance uses only the new tiny tree; never repeats old scan.
        fixture=ROOT/'tmp/fixture/oversize.stdout';fixture.parent.mkdir();fixture.write_bytes(b'fixture')
        capture=ROOT/'tmp/capture/oversize.stdout';capture.parent.mkdir();capture.write_bytes(b'capture');CAPTURES[str(capture.resolve())]='governance-capture'
        nt,nl=newscan();beforelogs=sum(max(p.stat().st_size,p.stat().st_blocks*512) for p in (ROOT/'logs/trace.jsonl',capture));require(nl==beforelogs,'full-path capture classification');require(nt>=nl+max(fixture.stat().st_size,fixture.stat().st_blocks*512),'fixture excluded from total')
        fixture.unlink();capture.unlink();del CAPTURES[str(capture.resolve())]
        p=subprocess.Popen(['sleep','30'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL);pidstart=identity(p.pid)
        try:raise ValueError('governance expected failure')
        except ValueError:pass
        finally:
            require(identity(p.pid)==pidstart,'governance PID identity');p.terminate();p.wait(timeout=5)
        event('governance_pass',root_disjoint=True,full_path_capture=True,fixture_total_only=True,failed_finally_reaped=True)
        payload=b'0123456789abcdef'*(1048576//16);(ROOT/'payload').mkdir();(ROOT/'payload/payload.bin').write_bytes(payload)
        env=dict(os.environ);env.update(TMPDIR=str(ROOT/'tmp'),TMP=str(ROOT/'tmp'),TEMP=str(ROOT/'tmp'),XDG_CACHE_HOME=str(ROOT/'cache'),PYTHONDONTWRITEBYTECODE='1',NO_PROXY='127.0.0.1,localhost',no_proxy='127.0.0.1,localhost')
        argv=[str(binary),'--port','0','--root',str(ROOT/'payload'),'--threads','2','--idle-timeout-ms','30000','--keep-alive-timeout-ms','15000','--shutdown-timeout-ms','5000','--metrics-on-exit']
        handles=[(ROOT/'logs/server.stdout').open('wb'),(ROOT/'logs/server.stderr').open('wb')];server=subprocess.Popen(argv,cwd=OLD/'source',env=env,stdout=handles[0],stderr=handles[1],start_new_session=True);server_id=identity(server.pid);state['owned_left']=1;state['argv']=argv;state['pid']=server.pid;state['starttime']=server_id
        event('server_start',pid=server.pid,starttime=server_id,argv=argv,payload_sha256=hashlib.sha256(payload).hexdigest())
        readyend=min(start+170,time.monotonic()+5)
        while server.poll() is None:
            guard(readyend,'readiness')
            match=re.search(r'listening on port (\d+)\.',(ROOT/'logs/server.stdout').read_text())
            if match:port=int(match[1]);require(listener(server.pid,server_id,port),'listener ownership');break
            time.sleep(.02)
        require(port is not None,'server exited before readiness');event('ready',port=port,listener_owned=True)
        peer=socket.create_connection(('127.0.0.1',port),timeout=2);peer.settimeout(2);event('socket_connected',local=peer.getsockname(),remote=peer.getpeername())
        request=b'GET /payload.bin HTTP/1.1\r\nHost: loopback\r\nConnection: keep-alive\r\n\r\n'
        for index in range(3):
            current=index;guard(start+170,f'request-{index}-begin');event('send_begin',index=index,bytes=len(request));peer.sendall(request);event('send_end',index=index,bytes=len(request));received=b''
            while b'\r\n\r\n' not in received:
                event('recv_begin',index=index,phase='header',cumulative=len(received));part=peer.recv(65536);event('recv_end',index=index,phase='header',bytes=len(part),cumulative=len(received)+len(part));require(part,'header EOF');received+=part
            header,body=received.split(b'\r\n\r\n',1);headers=dict(line.split(b':',1) for line in header.split(b'\r\n')[1:]);require(header.startswith(b'HTTP/1.1 200 '),'audit status');require(int(headers[b'Content-Length'])==len(payload),'Content-Length');require(headers[b'Connection'].strip()==b'keep-alive','keepalive');event('header_complete',index=index,header_bytes=len(header)+4,body_bytes=len(body))
            while len(body)<len(payload):
                event('recv_begin',index=index,phase='body',cumulative=len(body));part=peer.recv(65536);event('recv_end',index=index,phase='body',bytes=len(part),cumulative=len(body)+len(part));require(part,'body EOF');body+=part;guard(start+170,f'request-{index}-body')
            require(body==payload,'body hash or tail mismatch');event('tail_begin',index=index);readable=select.select([peer],[],[],.02)[0];event('tail_end',index=index,readable=bool(readable));require(not readable,'unsolicited bytes or premature close');obs={'index':index,'status':200,'content_length':len(body),'sha256':hashlib.sha256(body).hexdigest()};state['completed_requests'].append(obs);event('request_complete',**obs)
        state['status']='not_reproduced';event('audit_complete',count=3)
    except Exception as exc:
        error=f'{type(exc).__name__}: {exc}';state['status']='observed_failure';state['failure']=error;snapshot(error)
    finally:
        if peer is not None:peer.close();event('socket_closed')
        if server is not None:
            if server.poll() is None and identity(server.pid)==server_id:
                server.send_signal(signal.SIGTERM)
                try:server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    forced=True;server.kill();server.wait(timeout=2)
            else:server.wait(timeout=2)
            state['owned_left']=int(server.poll() is None);state['server_exit']=server.returncode;state['forced']=forced;event('server_reaped',exit=server.returncode,forced=forced)
        for h in handles:h.close()
        if (ROOT/'payload').exists():shutil.rmtree(ROOT/'payload')
        stdout=(ROOT/'logs/server.stdout').read_text() if (ROOT/'logs/server.stdout').exists() else ''
        block=re.search(r'HP_METRICS_BEGIN\n(.*?)HP_METRICS_END',stdout,re.S);state['metrics']={x.split()[0]:int(x.split()[1]) for x in block.group(1).splitlines() if len(x.split())==2} if block else {}
        event('cleanup_complete',owned_left=state['owned_left'],payload_absent=not (ROOT/'payload').exists(),metrics=state['metrics'])
        after=inventory(OLD);before=json.loads((MAN/'old-before.json').read_text());state['old_tree_unchanged']=after==before;save(MAN/'old-after.json',after)
        state['guards']=guards;state['persistence_intervals']=writes;state['max_scan_s']=max((x['scan_end']-x['scan_begin'] for x in guards),default=0);state['max_guard_interval_s']=max((b['begin']-a['begin'] for a,b in zip(guards,guards[1:])),default=0);state['max_persistence_s']=max((x['end']-x['start'] for x in writes),default=0)
        # Final settlement scans the new tree only: no extra frozen-tree observer pause.
        nt,nl=newscan();lastold=guards[-1] if guards else {'old_total':before['charge'],'old_logs':0};state['final_total_bytes']=lastold['old_total']+nt;state['final_log_bytes']=lastold['old_logs']+nl;state['peak_total_bytes']=max(state['peak_total_bytes'],state['final_total_bytes']);state['peak_log_bytes']=max(state['peak_log_bytes'],state['final_log_bytes']);state['elapsed_s']=time.monotonic()-start;state['diagnostic_within_180s']=state['elapsed_s']<=180;state['RAC_D01']=state['old_tree_unchanged'] and state['owned_left']==0 and not forced and state['diagnostic_within_180s'] and state['peak_total_bytes']<=4*1024**3 and state['peak_log_bytes']<=512*1024**2
        save(MAN/'state.json',state);event('settled',status=state['status'],elapsed_s=state['elapsed_s'],RAC_D01=state['RAC_D01']);trace.close()
        # Include final state+trace allocation in the final maximum.
        nt,nl=newscan();state['final_total_bytes']=lastold['old_total']+nt;state['final_log_bytes']=lastold['old_logs']+nl;state['peak_total_bytes']=max(state['peak_total_bytes'],state['final_total_bytes']);state['peak_log_bytes']=max(state['peak_log_bytes'],state['final_log_bytes']);state['elapsed_s']=time.monotonic()-start;save(MAN/'state.json',state)
        print(json.dumps({k:state[k] for k in ('status','failure','elapsed_s','RAC_D01','max_scan_s','max_guard_interval_s','owned_left','old_tree_unchanged','started_utc','role_expiry_utc') if k in state}))
if __name__=='__main__':
    require(len(sys.argv)==2 and sys.argv[1] in ('prepare','execute'),'explicit mode required')
    prepare() if sys.argv[1]=='prepare' else execute()
