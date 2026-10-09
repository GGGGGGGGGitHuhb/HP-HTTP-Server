#!/usr/bin/env python3
"""Single approved V1.0/S3 R001 Builder batch; not a permanent benchmark framework."""
import copy, ctypes, hashlib, json, math, os, pathlib, re, resource, shutil
import signal, socket, statistics, subprocess, sys, time

REPO = pathlib.Path.cwd().resolve()
ROOT = REPO / '.cache/v1.0-s3/builder/rework-001'
SOURCE = ROOT / 'source'
CANDIDATE = '20bd03142b4c828a7e939c30b501812da2bd5440'
WRK = REPO / '.cache/v0.5-s4/tools/root/usr/bin/wrk'
LIB = REPO / '.cache/v0.5-s4/tools/root/usr/lib/x86_64-linux-gnu'
LUA = REPO / 'benchmark/matrix/Summary.lua'
GENERATED = [SOURCE / x for x in ('build', 'build-release', 'build-asan', '.validation')]
ENV = dict(os.environ)
for key in ('CXXFLAGS', 'CPPFLAGS', 'LDFLAGS', 'HP_S3_TEST_TMP_ROOT', 'HP_MATRIX_TEST_TMP_ROOT', 'HP_ANALYSIS_TEST_TMP_ROOT', 'ASAN_OPTIONS', 'UBSAN_OPTIONS'):
    ENV.pop(key, None)
ENV.update(TMPDIR=str(ROOT/'tmp'), TMP=str(ROOT/'tmp'), TEMP=str(ROOT/'tmp'), XDG_CACHE_HOME=str(ROOT/'cache'), PYTHONDONTWRITEBYTECODE='1', NO_PROXY='127.0.0.1,localhost', no_proxy='127.0.0.1,localhost')
OWNED = []
STATE = {}

def require(condition, message):
    if not condition:
        raise ValueError(message)

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def save(path, data):
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False, allow_nan=False)+'\n')

def identity(pid):
    try:
        text = pathlib.Path(f'/proc/{pid}/stat').read_text()
        return text[text.rfind(')')+2:].split()[19]
    except FileNotFoundError:
        return None

def scan():
    total = logs = 0
    classifications = {}
    fixture = SOURCE / 'build/test-tmp'
    for path in ROOT.rglob('*'):
        if not path.is_file() or path.is_symlink():
            continue
        try:
            st = path.stat()
        except FileNotFoundError:
            continue
        charge = max(st.st_size, st.st_blocks*512)
        total += charge
        resolved = path.resolve()
        captured = str(resolved) in STATE.get('captures', {})
        # These exact generated trees have known CMake/CTest and frozen test producers.
        for build in GENERATED[:3]:
            if resolved.is_relative_to(build.resolve()):
                rel = resolved.relative_to(build.resolve())
                if rel.parts[0] == 'Testing' or rel.as_posix() in ('CMakeFiles/CMakeConfigureLog.yaml','CMakeFiles/CMakeOutput.log','CMakeFiles/CMakeError.log'):
                    captured = True
                if rel.parts[0] == 'test-tmp' and path.suffix in ('.stdout','.stderr','.log'):
                    # Approved negative fixtures are inputs, not process captures.
                    sub = rel.parts[1:]
                    synthetic = (len(sub)==2 and sub[0].startswith('synthetic-') and sub[1]=='oversize.stdout') or (len(sub)==3 and sub[0].startswith('synthetic-') and tuple(sub[1:]) in [('sample-01','server.stderr'),('sample-02','measurement.stdout')])
                    captured = not synthetic
                    classifications[str(path.relative_to(ROOT))] = 'frozen-test-input' if synthetic else 'frozen-test-process-capture'
        if resolved.is_relative_to((SOURCE/'.validation/smoke').resolve()) and path.name == 'server.log':
            captured = True
        if captured:
            logs += charge
    return total, logs, classifications

def guard(deadline=None, total_limit=4*1024**3, log_limit=512*1024**2):
    total, logs, categories = scan()
    STATE['peak_total_bytes'] = max(total, STATE.get('peak_total_bytes',0))
    STATE['peak_log_bytes'] = max(logs, STATE.get('peak_log_bytes',0))
    STATE.setdefault('dynamic_classifications',{}).update(categories)
    now = time.monotonic()
    require(now < STATE['started_monotonic']+1790, 'global deadline (10s cleanup reserved)')
    require(deadline is None or now < deadline, 'phase/command deadline')
    require(total <= total_limit, 'role total byte limit')
    require(logs <= log_limit, 'real capture byte limit')

def descendants(pid):
    found = []
    for path in pathlib.Path('/proc').glob('[0-9]*/stat'):
        try:
            rest = path.read_text().rsplit(')',1)[1].split()
            if int(rest[1]) == pid:
                child = int(path.parent.name)
                found.append((child, identity(child)))
                found.extend(descendants(child))
        except (FileNotFoundError, ProcessLookupError):
            pass
    return found

def launch(argv, label, cwd=SOURCE, extra=None):
    prefix = ROOT/'logs'/label
    out, err = prefix.with_suffix('.stdout'), prefix.with_suffix('.stderr')
    handles = [out.open('wb'), err.open('wb')]
    env = dict(ENV); env.update(extra or {})
    p = subprocess.Popen([str(x) for x in argv], cwd=cwd, env=env, stdout=handles[0], stderr=handles[1], start_new_session=True)
    entry = dict(argv=[str(x) for x in argv], cwd=str(cwd), env={k:env[k] for k in ('TMPDIR','TMP','TEMP','XDG_CACHE_HOME','PYTHONDONTWRITEBYTECODE','NO_PROXY','no_proxy')}, pid=p.pid, starttime=identity(p.pid), stdout=str(out), stderr=str(err), children={})
    entry['env'].update(extra or {})
    STATE.setdefault('commands',[]).append(entry)
    STATE.setdefault('captures',{}).update({str(out.resolve()):label, str(err.resolve()):label})
    obj = (p, handles, entry)
    OWNED.append(obj)
    return obj

def reap(obj, requested_signal=None):
    p, handles, entry = obj
    forced = False
    if requested_signal is not None and p.poll() is None:
        require(identity(p.pid) == entry['starttime'], 'PID identity changed')
        p.send_signal(requested_signal)
    if p.poll() is None:
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            require(identity(p.pid) == entry['starttime'], 'PID identity changed before kill')
            p.kill(); forced=True; p.wait(timeout=2)
    else:
        p.wait()
    survivors = []
    for pid, start in entry['children'].items():
        if identity(int(pid)) == start:
            survivors.append((int(pid),start))
    for pid, start in survivors:
        if identity(pid)==start:
            os.kill(pid, signal.SIGTERM)
    cleanup_deadline = time.monotonic()+1
    for pid,start in survivors:
        while identity(pid)==start and time.monotonic()<cleanup_deadline:
            try:
                if os.waitpid(pid,os.WNOHANG)[0]: break
            except ChildProcessError: pass
            time.sleep(.01)
        if identity(pid)==start:
            os.kill(pid,signal.SIGKILL); forced=True
        try: os.waitpid(pid,0)
        except ChildProcessError: pass
    for handle in handles: handle.close()
    entry.update(returncode=p.returncode, forced=forced, reaped=True, descendant_survivors=len(survivors))
    OWNED.remove(obj)
    require(not forced, 'forced cleanup invalidates operation')
    require(not survivors, 'descendants survived owning command')
    return entry

def wait_command(obj, deadline, acceptable=(0,)):
    p, _, entry = obj
    began=time.monotonic()
    try:
        while p.poll() is None:
            for pid,start in descendants(p.pid): entry['children'][str(pid)]=start
            guard(deadline)
            time.sleep(.1)
        result=reap(obj)
        result['elapsed_s']=time.monotonic()-began
        require(result['returncode'] in acceptable, 'nonzero exit '+str(result['argv'])+': '+str(result['returncode']))
        return result
    finally:
        if obj in OWNED: reap(obj,signal.SIGTERM)

def command(argv,label,deadline,cwd=SOURCE,extra=None,acceptable=(0,)):
    return wait_command(launch(argv,label,cwd,extra),deadline,acceptable)

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

def verify_source():
    baseline=json.loads((ROOT/'manifest/source-files.json').read_text())
    observed={str(p.relative_to(SOURCE)):sha(p) for p in SOURCE.rglob('*') if p.is_file() and not any(p.is_relative_to(g) for g in GENERATED)}
    require(baseline==observed,'original archive file drift or unexpected output')

def synthetic(deadline):
    results=[]
    sample=dict(schema=2,requests=100,duration_us=1000000,bytes=102400,errors=dict(connect=0,read=0,write=0,status=0,timeout=0),latency_us=dict(mean=2,p50=1,p95=3,p99=4,max=5),latency_distribution='wrk_corrected',population_status='not_collected',corrected_population=None,nonzero_bins=None,correction_interval_us=320000)
    encoded='MATRIX_SUMMARY '+json.dumps(sample)
    parse_summary(encoded,0,1024,32)
    cases=[]
    for key in ('requests','duration_us','bytes','errors','latency_us','corrected_population'):
        altered=copy.deepcopy(sample); del altered[key]; cases.append((key,json.dumps(altered)))
    altered=copy.deepcopy(sample); altered['errors']['timeout']=1; cases.append(('error',json.dumps(altered)))
    altered=copy.deepcopy(sample); altered['requests']=0; cases.append(('count-zero',json.dumps(altered)))
    altered=copy.deepcopy(sample); altered['latency_us']['p99']=.5; cases.append(('ordering',json.dumps(altered)))
    for name,text in cases:
        try: parse_summary('MATRIX_SUMMARY '+text,0,1024,32)
        except (ValueError,KeyError): results.append(name)
        else: raise ValueError('synthetic accepted '+name)
    for name,text,code in [('duplicate',encoded+'\n'+encoded,0),('missing','',0),('nonzero',encoded,1)]:
        try: parse_summary(text,code,1024,32)
        except ValueError: results.append(name)
        else: raise ValueError('synthetic accepted '+name)
    require(all(not p.exists() for p in GENERATED),'generated tree pre-created')
    non_target=ROOT/'tmp/same-name/build'; non_target.mkdir(parents=True)
    require(non_target.resolve() not in [p.resolve() for p in GENERATED],'basename ownership')
    fixture=ROOT/'tmp/oversize.stdout'; fixture.write_bytes(b'fixture')
    require(str(fixture.resolve()) not in STATE.get('captures',{}),'fixture capture classification')
    # Exact registered real capture exceeds a deliberately small synthetic limit.
    obj=launch(['/usr/bin/sleep','20'],'synthetic-owned-sleep')
    capture=pathlib.Path(obj[2]['stdout']); capture.write_bytes(b'x'*4096)
    try:
        guard(deadline,log_limit=1)
    except ValueError as exc: require(str(exc)=='real capture byte limit','wrong quota branch'); results.append('output-stop')
    finally: reap(obj,signal.SIGTERM)
    obj=launch(['/usr/bin/sleep','20'],'synthetic-deadline-sleep')
    try: guard(time.monotonic()-1)
    except ValueError as exc: require(str(exc)=='phase/command deadline','wrong deadline branch'); results.append('deadline-stop')
    finally: reap(obj,signal.SIGTERM)
    results.extend(['canonical-same-name-non-target','generated-initial-state','fixture-classification','failure-finally-reap'])
    save(ROOT/'manifest/synthetic.json',dict(passed=results,owned_left=len(OWNED)))

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

def readiness(obj,deadline):
    end=min(deadline,time.monotonic()+5)
    while obj[0].poll() is None:
        guard(end)
        match=re.search(r'listening on port (\d+)\.',pathlib.Path(obj[2]['stdout']).read_text())
        if match:
            port=int(match[1]); require(owned_listener(obj,port),'listener ownership'); return port
        time.sleep(.02)
    raise ValueError('server exited before readiness')

def audit(port,payload,deadline):
    observations=[]
    with socket.create_connection(('127.0.0.1',port),timeout=2) as peer:
        peer.settimeout(2)
        for index in range(3):
            guard(deadline)
            peer.sendall(b'GET /payload.bin HTTP/1.1\r\nHost: loopback\r\nConnection: keep-alive\r\n\r\n')
            received=b''
            while b'\r\n\r\n' not in received:
                part=peer.recv(65536); require(part,'header EOF'); received+=part
            header,body=received.split(b'\r\n\r\n',1)
            require(header.startswith(b'HTTP/1.1 200 '),'audit status')
            headers=dict(line.split(b':',1) for line in header.split(b'\r\n')[1:])
            require(int(headers[b'Content-Length'])==len(payload),'Content-Length')
            require(headers[b'Connection'].strip()==b'keep-alive','keepalive')
            while len(body)<len(payload):
                part=peer.recv(65536); require(part,'body EOF'); body+=part; guard(deadline)
            require(body==payload,'body hash or tail mismatch')
            readable=__import__('select').select([peer],[],[],.02)[0]
            require(not readable,'unsolicited bytes or premature close')
            observations.append(dict(status=200,content_length=len(body),sha256=hashlib.sha256(body).hexdigest(),same_socket_index=index))
    return observations

def metrics(text):
    require(text.count('HP_METRICS_BEGIN')==text.count('HP_METRICS_END')==1,'metrics markers')
    values=dict(line.split() for line in text.split('HP_METRICS_BEGIN\n')[1].split('HP_METRICS_END')[0].splitlines())
    values={key:int(val) for key,val in values.items()}
    require(values['connections_active']==values['logger_pending']==0,'final active resources')
    require(values['requests_started_total']==values['responses_completed_total']+values['requests_aborted_total']==values['latency_count'],'final accounting')
    return values

def sample(name,config,warm,duration,deadline):
    manifest=json.loads((ROOT/'manifest/release.json').read_text())
    binary=pathlib.Path(manifest['binary']); require(binary==SOURCE/'build-release/hp_http_server' and sha(binary)==manifest['binary_sha256'] and manifest['candidate']==CANDIDATE and manifest['patch_sha256']==sha(ROOT/'manifest/candidate.patch'),'Release manifest input')
    payload=(b'0123456789abcdef'*(config['bytes']//16))
    payloadroot=SOURCE/'.validation'/name; require(not payloadroot.exists(),'sample root exists'); payloadroot.mkdir()
    (payloadroot/'payload.bin').write_bytes(payload)
    row=dict(name=name,config=config,status='invalid')
    STATE['samples'].append(row); save(ROOT/'manifest/state.json',STATE)
    server=launch([binary,'--port','0','--root',payloadroot,'--threads',str(config['workers']),'--idle-timeout-ms','30000','--keep-alive-timeout-ms','15000','--shutdown-timeout-ms','5000','--metrics-on-exit'],name+'-server')
    try:
        port=readiness(server,deadline); row['port']=port; row['server_identity']=dict(pid=server[2]['pid'],starttime=server[2]['starttime'],listener_owned=True)
        row['pre_audit']=audit(port,payload,deadline)
        for phase,seconds in [('warmup',warm),('measurement',duration)]:
            argv=[WRK,'-t',str(config['threads']),'-c',str(config['connections']),'--timeout','2s','--latency','-d',str(seconds)+'s','-s',LUA,f'http://127.0.0.1:{port}/payload.bin']
            result=command(argv,name+'-'+phase,deadline,extra=dict(LD_LIBRARY_PATH=str(LIB),HP_MATRIX_MODE='keepalive',HP_MATRIX_CONNECTIONS=str(config['connections'])))
            text=pathlib.Path(result['stdout']).read_text()
            row[phase]=parse_summary(text,result['returncode'],len(payload),config['connections'])
            row[phase+'_raw_stdout']=text
        row['post_audit']=audit(port,payload,deadline)
        row['cleanup']=reap(server,signal.SIGTERM); require(row['cleanup']['returncode']==0,'server normal exit')
        row['metrics']=metrics(pathlib.Path(server[2]['stdout']).read_text())
        with socket.socket() as probe:
            probe.settimeout(.2); require(probe.connect_ex(('127.0.0.1',port))!=0,'port remains listening')
        row['status']='valid'
    except Exception as exc:
        row['failure']=str(exc); raise
    finally:
        if server in OWNED: row['cleanup']=reap(server,signal.SIGTERM)
        shutil.rmtree(payloadroot)
        save(ROOT/'performance'/f'{name}.json',row)
        save(ROOT/'manifest/state.json',STATE)

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

def prepare():
    require(not (ROOT/'manifest/state.json').exists(),'single batch already started')
    require(all(not p.exists() for p in GENERATED),'initial generated tree exists')
    baseline={str(p.relative_to(SOURCE)):sha(p) for p in SOURCE.rglob('*') if p.is_file()}
    save(ROOT/'manifest/source-files.json',baseline)
    require(sha(WRK)=='b10e53769443c2bf3be2cdedec8ef6571aa5bfd1494796b247f3f9296e3af71d','wrk hash')
    require(sha(LUA)=='0706c8defa1a759eda90185ca5c9f02d755bb3c5a7248367d5f0c3618111340e','Lua hash')
    tools={str(p):sha(p) for p in [WRK,LUA]+[p for p in LIB.iterdir() if p.is_file()]}
    save(ROOT/'manifest/inputs.json',dict(candidate=CANDIDATE,tree=subprocess.check_output(['git','rev-parse',CANDIDATE+'^{tree}'],cwd=REPO,text=True).strip(),archive_sha256=sha(ROOT/'source.tar'),patch_sha256=sha(ROOT/'manifest/candidate.patch'),candidate_files_sha256=sha(ROOT/'manifest/candidate-files.json'),original_file_count=len(baseline),tools=tools,generated_trees=[str(p) for p in GENERATED],initial_generated_exist=[p.exists() for p in GENERATED]))
    require(shutil.disk_usage(ROOT).free>=4*1024**3,'free disk')
    mem=int(re.search(r'MemAvailable:\s+(\d+)',pathlib.Path('/proc/meminfo').read_text())[1])*1024
    require(mem>=1024**3,'MemAvailable'); require(resource.getrlimit(resource.RLIMIT_NOFILE)[0]>=256,'nofile')
    save(ROOT/'manifest/static-admission.json',dict(free_disk_bytes=shutil.disk_usage(ROOT).free,mem_available_bytes=mem,nofile=resource.getrlimit(resource.RLIMIT_NOFILE),script_sha256=sha(pathlib.Path(__file__)),native_route=str(REPO),budget_s=1800,previous_batch_elapsed_s=146.0728357490152,cleanup_reserved_s=10,build_cap_s=600,functional_cap_s=600,performance_cap_s=360,total_limit_bytes=4*1024**3,capture_limit_bytes=512*1024**2))
    print('STATIC_READY',flush=True)

def main():
    global STATE
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    ctypes.CDLL(None).prctl(36,1,0,0,0) # subreaper: own and wait adopted descendants on failure
    phase=sys.argv[1]
    if phase=='prepare': prepare(); return
    statepath=ROOT/'manifest/state.json'
    if phase=='build-debug':
        require(not statepath.exists(),'batch restart forbidden')
        STATE=dict(previous_batch_elapsed_s=146.0728357490152,started_monotonic=time.monotonic(),started_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),status='running',phases=[],commands=[],captures={},samples=[],planned_formal=['r1-M2','r1-M3','r1-M6','r2-M6','r2-M3','r2-M2','r3-M3','r3-M2','r3-M6'])
    else:
        STATE=json.loads(statepath.read_text()); require(STATE['status']=='running','failed/final batch cannot continue')
    try:
        guard(); STATE['current_phase']=phase; save(statepath,STATE)
        if phase=='build-debug':
            synthetic(time.monotonic()+60)
            checker_probe(time.monotonic()+60)
            for tool,args in [('cmake',['--version']),('g++',['--version']),('python3',['--version']),('curl',['--version']),('bash',['--version'])]: command([tool]+args,'version-'+tool,time.monotonic()+10,cwd=ROOT)
            command([WRK,'--version'],'version-wrk',time.monotonic()+10,cwd=ROOT,extra=dict(LD_LIBRARY_PATH=str(LIB)),acceptable=(0,1))
            build('debug')
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
            aggregates={}
            for scenario in configs:
                rows=[row['measurement'] for row in STATE['samples'] if row['name'].startswith('r') and row['name'].endswith(scenario)]
                aggregates[scenario]={key:dict(median=statistics.median(vals),min=min(vals),max=max(vals)) for key,vals in [('qps',[r['qps'] for r in rows]),('received_mib_s',[r['received_mib_s'] for r in rows]),('corrected_p99_us',[r['latency_us']['p99'] for r in rows])]}
            STATE['aggregates']=aggregates; STATE['status']='passed'
        else: raise ValueError('unknown phase')
        verify_source(); STATE['phases'].append(dict(phase=phase,status='passed',completed_monotonic=time.monotonic()))
    except Exception as exc:
        STATE['status']='failed'; STATE['failure']=str(exc); STATE['phases'].append(dict(phase=phase,status='failed',failure=str(exc)))
        print('FAILED',phase,str(exc),flush=True)
    finally:
        for obj in list(OWNED):
            try: reap(obj,signal.SIGTERM)
            except Exception as exc: STATE.setdefault('cleanup_failures',[]).append(str(exc))
        total,logs,classification=scan(); STATE['peak_total_bytes']=max(total,STATE.get('peak_total_bytes',0)); STATE['peak_log_bytes']=max(logs,STATE.get('peak_log_bytes',0)); STATE.update(final_total_bytes=total,final_capture_bytes=logs,elapsed_s=time.monotonic()-STATE['started_monotonic'],owned_left=len(OWNED))
        STATE['not_run']=[name for name in STATE['planned_formal'] if name not in [r['name'] for r in STATE['samples']]]
        save(statepath,STATE)
        print(json.dumps({key:STATE.get(key) for key in ('current_phase','status','failure','elapsed_s','peak_total_bytes','peak_log_bytes','final_total_bytes','owned_left')},ensure_ascii=False),flush=True)
    sys.exit(1 if STATE['status']=='failed' else 0)

if __name__=='__main__': main()
