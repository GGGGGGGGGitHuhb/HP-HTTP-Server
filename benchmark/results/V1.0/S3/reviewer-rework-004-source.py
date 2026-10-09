#!/usr/bin/env python3
"""Independent V1.0/S3 reviewer driver. Fixed approval, no retries or refill."""
import ast, errno, select, stat, datetime, hashlib, json, math, os, pathlib, re, resource, shutil, signal, socket, statistics, subprocess, sys, tarfile, time
R=pathlib.Path(__file__).resolve().parents[1]
REPO=R.parents[3]
SRC=R/'source'
COMMIT='20bd03142b4c828a7e939c30b501812da2bd5440'
WRK=REPO/'.cache/v0.5-s4/tools/root/usr/bin/wrk'
LIB=REPO/'.cache/v0.5-s4/tools/root/usr/lib/x86_64-linux-gnu'
LUA=SRC/'benchmark/matrix/Summary.lua'
SAN=['async_logger_batch_tests','r6_callbacks_tests','tcp_nodelay_tests','server_metrics_tests','http_observability_tests']
ORDER=['M2','M3','M6','M6','M3','M2','M3','M2','M6']
SCENES={'M2':(1024,2,2,32),'M3':(1024,4,4,128),'M6':(1048576,2,2,32)}
GEN=['build','build-release','build-asan','.validation']
ENV=os.environ.copy()
for k in ['CXXFLAGS','CFLAGS','CPPFLAGS','LDFLAGS','HP_S3_TEST_TMP_ROOT','HP_MATRIX_TEST_TMP_ROOT','HP_ANALYSIS_TEST_TMP_ROOT','ASAN_OPTIONS','UBSAN_OPTIONS']:
    ENV.pop(k,None)
ENV.update(TMPDIR=str(R/'tmp'),TMP=str(R/'tmp'),TEMP=str(R/'tmp'),XDG_CACHE_HOME=str(R/'cache'),PYTHONDONTWRITEBYTECODE='1',NO_PROXY='127.0.0.1,localhost',no_proxy='127.0.0.1,localhost')
STATE=R/'manifest/state.json'
PUBLIC=[REPO/'benchmark/results/V1.0/S3'/x for x in ('reviewer-rework-004.json','reviewer-rework-004.md','reviewer-rework-004-source.py')]+[REPO/'docs/reviewer/reports/V1.0/S3-report-005.md']
class Reject(Exception):pass
def sha(p,tick=None):
    h=hashlib.sha256()
    with open(p,'rb') as f:
        for block in iter(lambda:f.read(1024*1024),b''):
            h.update(block)
            if tick:tick()
    return h.hexdigest()
def save(p,obj):
    p.write_text(json.dumps(obj,ensure_ascii=False,indent=2)+'\n')
def pidstart(pid):
    try:return pathlib.Path('/proc',str(pid),'stat').read_text().rsplit(')',1)[1].split()[19]
    except FileNotFoundError:return None
def parse_summary(text,size=None,connections=None):
    rows=[x[len('MATRIX_SUMMARY '):] for x in text.splitlines() if x.startswith('MATRIX_SUMMARY ')]
    if len(rows)!=1:raise Reject('summary count')
    def pairs(ps):
        d={}
        for k,v in ps:
            if k in d:raise Reject('duplicate JSON key')
            d[k]=v
        return d
    d=json.loads(rows[0],object_pairs_hook=pairs,parse_constant=lambda x:(_ for _ in ()).throw(Reject('nonfinite')))
    if d['schema']!=2 or d['latency_distribution']!='wrk_corrected' or d['population_status']!='not_collected':raise Reject('schema')
    if d['corrected_population'] is not None or d['nonzero_bins'] is not None:raise Reject('population')
    for key in ['duration_us','requests','bytes','correction_interval_us']:
        if isinstance(d[key],bool) or not isinstance(d[key],(int,float)) or not math.isfinite(d[key]) or d[key]<=0:raise Reject('invalid count')
    if set(d['errors'])!={'connect','read','write','status','timeout'} or any(type(x) not in (int,float) or x!=0 for x in d['errors'].values()):raise Reject('wrk errors')
    lat=d['latency_us']
    if set(lat)!={'mean','p50','p95','p99','max'} or any(type(x) not in (int,float) or not math.isfinite(x) or x<0 for x in lat.values()):raise Reject('latency')
    if not lat['p50']<=lat['p95']<=lat['p99']<=lat['max'] or lat['mean']>lat['max']:raise Reject('latency order')
    if any(type(d[k]) not in (int,float) or int(d[k])!=d[k] for k in ('requests','duration_us','bytes')):raise Reject('integer counts')
    if size is not None and d['bytes']<d['requests']*size:raise Reject('body accounting')
    if connections is not None and abs(d['correction_interval_us']-d['duration_us']*connections/d['requests'])>1e-5:raise Reject('correction interval')
    d['qps']=d['requests']*1e6/d['duration_us'];d['received_mib_s']=d['bytes']*1e6/d['duration_us']/1048576
    return d
class Driver:
    def __init__(self):
        self.s=json.loads(STATE.read_text()) if STATE.exists() else {'commands':[],'children':[],'samples':[{'id':i+1,'scene':x,'status':'NotRun'} for i,x in enumerate(ORDER)],'capture_paths':{},'peak_total':0,'peak_log':0,'status':'prepared','phase_seconds':{'functional':0},'candidate':COMMIT,'monitor_scans':[],'active_windows':[]}
        if self.s['status']=='stopped' or (self.s['status']=='dynamic-complete' and (sys.argv[1]!='package' or self.s.get('packaged'))):raise Reject('closed batch cannot resume')
        self.live={};self.phase=None;self.phase_start=None;self.func_base=self.s['phase_seconds'].get('functional',0);self.window_start=time.monotonic();self.last_scan=None;self.next_tick=0;self.governance_error=None;self.trace=None;self.scan_source=SRC;self.generated=[SRC/x for x in GEN[:3]];self.cleaning=False
        if 'start' in self.s:self.begin_window()
    def begin_window(self):
        self.window_start=time.monotonic();self.last_scan=None
        self.trace=(R/'logs/trace.jsonl').open('a',buffering=1);self.register_capture(R/'logs/trace.jsonl','incremental reviewer trace')
        self.next_tick=self.s['start']+(int((self.window_start-self.s['start'])/.25)+1)*.25
        self.take_scan(force=True)
    def event(self,kind,**data):
        if self.trace:
            t=time.monotonic();self.trace.write(json.dumps({'kind':kind,'mono':t,'phase':self.phase,**data},separators=(',',':'))+'\n');self.trace.flush()
            self.s['max_trace_persist_s']=max(time.monotonic()-t,self.s.get('max_trace_persist_s',0))
    def park(self,failed=False):
        # The existing failure remains latched; finalization never turns it into PASS.
        self.cleaning=failed
        if self.live or any(pidstart(c['pid'])==c['starttime'] for c in self.s['children']):raise Reject('handoff still has external producers')
        if failed:
            observations={}
            for row in self.s['commands']:
                if row['label'].endswith('-server') and row.get('reaped'):
                    self.guard(cleanup=True)
                    try:observations[row['label']]={'metrics':self.metrics(row)}
                    except Exception as e:observations[row['label']]={'metrics_error':repr(e)}
            self.s['failure_exit_metrics']=observations
        closing={'start':self.window_start,'phase':self.phase,'closing_marker_monotonic':time.monotonic(),'terminal_evidence':'tool stdout; closing marker is not actual active end'}
        self.s['active_windows'].append(closing)
        self.persist();self.take_scan(force=True,cleanup=failed)
        if self.trace:self.trace.close();self.trace=None
        self.s['terminal_status']='failure' if failed else 'success'
        self.s['closing_charge_snapshot_s']=time.monotonic()-self.s['start']
        self.persist()
        # Last owned write is above. The real scan and all following updates are memory only.
        self.take_scan(force=True,cleanup=failed)
        self.guard(cleanup=failed)
        end=time.monotonic()
        if end-self.last_scan['start']>1 or end-self.last_scan['complete']>1:raise Reject('active end boundary >1s')
        closing['actual_end_monotonic']=end
        closing['last_scan_start']=self.last_scan['start'];closing['last_scan_complete']=self.last_scan['complete']
        self.s['terminal_complete']=True

    def persist(self):
        if 'start' in self.s:self.guard(cleanup=self.cleaning)
        save(STATE,self.s)
        if 'start' in self.s:self.guard(cleanup=self.cleaning)
    def admit(self):
        if 'start' in self.s:raise Reject('dynamic batch already started')
        identity_record=json.loads((R/'manifest/identity.json').read_text())
        if sha(pathlib.Path(__file__))!=identity_record['script_sha256']:raise Reject('frozen driver identity drift')
        if any((SRC/g).exists() for g in GEN):raise Reject('generated root exists before dynamic admission')
        self.s['start']=time.monotonic();self.s['started_utc']=datetime.datetime.now(datetime.timezone.utc).isoformat();self.s['status']='running'
        self.begin_window();self.persist()
        free=shutil.disk_usage(R).free;mem=int(re.search(r'MemAvailable:\s+(\d+)',pathlib.Path('/proc/meminfo').read_text())[1])*1024;limit=resource.getrlimit(resource.RLIMIT_NOFILE)[0]
        self.s['startup']={'free_disk':free,'mem_available':mem,'nofile':limit}
        if free<4*1024**3 or mem<1024**3 or limit<256:raise Reject('startup resource gate')
        print('NEW CLOCK',self.s['started_utc'],self.s['start'],flush=True)

    def setphase(self,name):
        if self.phase_start is not None:
            self.guard()
            if self.phase=='functional':self.s['phase_seconds']['functional']=self.func_base+time.monotonic()-self.phase_start
            else:self.s['phase_seconds'][self.phase]=time.monotonic()-self.phase_start
        self.phase=name;self.phase_start=time.monotonic();self.func_base=self.s['phase_seconds'].get('functional',0)
    def register_capture(self,path,producer):
        path=path.resolve()
        if not path.is_relative_to(R):raise Reject('capture path outside role')
        self.s['capture_paths'][str(path)]=producer
    def scan(self):
        total=logs=0;classes={}
        for base,dirs,files in os.walk(R,followlinks=False):
            for name in files:
                path=pathlib.Path(base,name)
                try:st=path.lstat()
                except FileNotFoundError:continue
                if not stat.S_ISREG(st.st_mode):continue
                charge=max(st.st_size,st.st_blocks*512);total+=charge;captured=str(path) in self.s['capture_paths']
                for build in self.generated:
                    if path.is_relative_to(build):
                        rel=path.relative_to(build)
                        if rel.parts[0]=='Testing' or rel.as_posix() in ('CMakeFiles/CMakeConfigureLog.yaml','CMakeFiles/CMakeOutput.log','CMakeFiles/CMakeError.log'):captured=True
                        if rel.parts[0]=='test-tmp' and path.suffix in ('.stdout','.stderr','.log'):
                            sub=rel.parts[1:];synthetic=(len(sub)==2 and sub[0].startswith('synthetic-') and sub[1]=='oversize.stdout') or (len(sub)==3 and sub[0].startswith('synthetic-') and tuple(sub[1:]) in [('sample-01','server.stderr'),('sample-02','measurement.stdout')])
                            captured=not synthetic;classes[str(path.relative_to(R))]='frozen-test-input' if synthetic else 'frozen-test-process-capture'
                if path.is_relative_to(self.scan_source/'.validation/smoke') and path.name=='server.log':captured=True
                if captured:logs+=charge
        for external in self.s.get('publication_outputs',[]):
            path=pathlib.Path(external)
            try:st=path.lstat()
            except FileNotFoundError:continue
            if not stat.S_ISREG(st.st_mode):raise Reject('declared publication output is not regular')
            total+=max(st.st_size,st.st_blocks*512)
        self.s['peak_total']=max(total,self.s['peak_total']);self.s['peak_log']=max(logs,self.s['peak_log']);self.s['final_total']=total;self.s['final_log']=logs;self.s['dynamic_classifications']=classes
        return total,logs
    def take_scan(self,force=False,cleanup=False):
        now=time.monotonic()
        if not force and now<self.next_tick:return
        begin=now;total,logs=self.scan();complete=time.monotonic();previous=self.last_scan
        row={'start':begin,'complete':complete,'duration':complete-begin,'total':total,'logs':logs,'active_start':self.window_start}
        if previous:row.update(start_interval=begin-previous['start'],complete_interval=complete-previous['complete'])
        else:row.update(start_interval=begin-self.window_start,complete_interval=complete-self.window_start)
        self.s['monitor_scans'].append(row);self.last_scan=row
        # Advance from the observed begin, never wait beyond a crossed tick.
        # The next guard makes one fresh scan; its begin skips all older grid ticks.
        self.next_tick=self.s['start']+(int((begin-self.s['start'])/.25)+1)*.25
        bad=max(row['duration'],row['start_interval'],row['complete_interval'])>1
        errors=[]
        if bad:errors.append('active scan start/complete/duration >1s')
        if total>4*1024**3 or logs>512*1024**2:errors.append('role output limit')
        for error in errors:self.s.setdefault('governance_failures',[]).append({'error':error,'scan_start':begin,'scan_complete':complete})
        if errors and self.governance_error is None:self.governance_error=errors[0];self.s['governance_error']=self.governance_error
        self.event('resource_scan',**row)
        if self.governance_error and not cleanup:raise Reject(self.governance_error)

    def guard(self,deadline=None,log_limit=512*1024**2,cleanup=False):
        self.take_scan(cleanup=cleanup)
        now=time.monotonic()
        if cleanup:
            if now>=self.s['start']+1800:raise Reject('cleanup global expiry')
            return
        if self.governance_error:raise Reject(self.governance_error)
        for pid,(process,row,_,_) in self.live.items():
            if process.poll() is None and pidstart(pid)!=row['starttime']:raise Reject('owned PID identity mismatch')
        if now-self.s['start']>=1790:raise Reject('global deadline (cleanup reserve)')
        if deadline is not None and now>=deadline:raise Reject('command deadline')
        if self.phase_start is not None:
            cap=360 if self.phase=='performance' else 1790 if self.phase=='publication' else 600;used=self.func_base if self.phase=='functional' else 0
            if now-self.phase_start+used>=cap:raise Reject('phase deadline')
        if self.s['final_log']>log_limit:raise Reject('role output limit')

    def spawn(self,argv,label,env=None,cwd=SRC):
        out=R/'logs'/f'{label}.stdout';err=R/'logs'/f'{label}.stderr'
        if out.exists() or err.exists():raise Reject('attempt overwrite evidence')
        self.register_capture(out,label);self.register_capture(err,label)
        oh=out.open('wb');eh=err.open('wb')
        p=subprocess.Popen([str(x) for x in argv],cwd=cwd,env=env or ENV,stdout=oh,stderr=eh,start_new_session=True)
        row={'label':label,'argv':[str(x) for x in argv],'cwd':str(cwd),'pid':p.pid,'starttime':pidstart(p.pid),'out':str(out),'err':str(err),'environment':{k:(env or ENV).get(k) for k in ['TMPDIR','TMP','TEMP','XDG_CACHE_HOME','PYTHONDONTWRITEBYTECODE','NO_PROXY','no_proxy','LD_LIBRARY_PATH','HP_S3_TEST_TMP_ROOT','HP_HTTP_TEST_THREADS','ASAN_OPTIONS','UBSAN_OPTIONS','HP_MATRIX_MODE','HP_MATRIX_CONNECTIONS','CXXFLAGS','LDFLAGS']},'started':time.monotonic(),'reaped':False,'forced':False}
        self.s['commands'].append(row);self.live[p.pid]=(p,row,oh,eh);self.persist();self.take_scan(force=True);return p,row
    def discover(self,parent):
        entries={}
        for pd in pathlib.Path('/proc').iterdir():
            if not pd.name.isdigit():continue
            try:v=(pd/'stat').read_text().rsplit(')',1)[1].split();entries[int(pd.name)]=(int(v[1]),v[19])
            except (FileNotFoundError,ProcessLookupError,PermissionError):continue
        owned={parent};changed=True
        while changed:
            changed=False
            for pid,(ppid,start) in entries.items():
                if ppid in owned and pid not in owned:owned.add(pid);changed=True
        for pid in owned-{parent}:
            child={'pid':pid,'starttime':entries[pid][1],'group':parent}
            if child not in self.s['children']:self.s['children'].append(child)

    def wait(self,p,row,timeout,expect=0):
        deadline=time.monotonic()+timeout
        while p.poll() is None:
            self.discover(p.pid);self.guard(deadline);time.sleep(min(.1,max(.001,self.next_tick-time.monotonic())))
        row['rc']=p.wait();row['finished']=time.monotonic();row['reaped']=True
        _,_,oh,eh=self.live.pop(p.pid);oh.close();eh.close();self.persist()
        if row['rc']!=expect:raise Reject(f"{row['label']} rc={row['rc']}")
        self.take_scan(force=True);self.guard();return pathlib.Path(row['out']).read_text(errors='replace')
    def command(self,argv,label,timeout=120,env=None,expect=0,cwd=SRC):
        p,row=self.spawn(argv,label,env,cwd);return self.wait(p,row,timeout,expect)
    def stop(self,p,row,sig=signal.SIGTERM):
        if p.poll() is None:
            if pidstart(p.pid)!=row['starttime']:raise Reject('PID identity drift')
            p.send_signal(sig);end=time.monotonic()+5
            while p.poll() is None and time.monotonic()<end:self.guard(cleanup=True);time.sleep(.05)
            if p.poll() is None:p.kill();row['forced']=True
        end=time.monotonic()+2
        while p.poll() is None and time.monotonic()<end:self.guard(cleanup=True);time.sleep(.05)
        if p.poll() is None:raise Reject('unreaped process')
        p.wait();row['rc']=p.returncode;row['reaped']=True;row['finished']=time.monotonic()
        if p.pid in self.live:
            _,_,oh,eh=self.live.pop(p.pid);oh.close();eh.close()
        self.persist();self.take_scan(force=True,cleanup=True)
        if self.governance_error:raise Reject(self.governance_error)
        if row['forced'] or row['rc']!=0:raise Reject('abnormal server shutdown')

    def cleanup(self):
        self.cleaning=True
        self.s['cleanup_started']=time.monotonic()
        for pid,(p,row,oh,eh) in list(self.live.items()):
            self.discover(pid)
            for ch in self.s['children']:
                if ch['group']==pid and pidstart(ch['pid'])==ch['starttime']:
                    try:os.kill(ch['pid'],signal.SIGTERM)
                    except ProcessLookupError:pass
            if p.poll() is None and pidstart(pid)==row['starttime']:p.terminate()
        end=min(time.monotonic()+5,self.s['start']+1797)
        while time.monotonic()<end and (any(p.poll() is None for p,_,_,_ in self.live.values()) or any(pidstart(ch['pid'])==ch['starttime'] for ch in self.s['children'])):
            self.guard(cleanup=True);time.sleep(.05)
        for pid,(p,row,oh,eh) in list(self.live.items()):
            if p.poll() is None and pidstart(pid)==row['starttime']:p.kill();row['forced']=True
        for ch in self.s['children']:
            if pidstart(ch['pid'])==ch['starttime']:
                try:os.kill(ch['pid'],signal.SIGKILL)
                except ProcessLookupError:pass
        end=min(time.monotonic()+2,self.s['start']+1800)
        while any(p.poll() is None for p,_,_,_ in self.live.values()) and time.monotonic()<end:self.guard(cleanup=True);time.sleep(.05)
        for pid,(p,row,oh,eh) in list(self.live.items()):
            row['reaped']=p.poll() is not None;row['rc']=p.returncode;oh.close();eh.close();self.live.pop(pid)
        self.s['remaining_children']=[x for x in self.s['children'] if pidstart(x['pid'])==x['starttime']]
        self.s['cleanup_finished']=time.monotonic();self.s['elapsed_including_cleanup']=self.s['cleanup_finished']-self.s['start'];self.persist();self.take_scan(force=True,cleanup=True)

    def readonly_final_check(self):
        identity=json.loads((R/'manifest/identity.json').read_text())
        if sha(WRK,self.guard)!=identity['wrk_sha256'] or sha(LUA,self.guard)!=identity['lua_sha256']:raise Reject('readonly tool final drift')
        if any(sha(pathlib.Path(p),self.guard)!=value for p,value in identity['library_files'].items()):raise Reject('readonly dependency drift')
        self.s['readonly_tools_unchanged']=True
    def source_check(self):
        pre=json.loads((R/'manifest/original-files.json').read_text());now={}
        for p in SRC.rglob('*'):
            if p.is_file() and not p.is_symlink():
                rel=p.relative_to(SRC)
                if rel.parts[0] not in GEN:now[str(rel)]=sha(p,self.guard);self.guard()
        if pre!=now:raise Reject('exported original drift or extra undeclared files')
        self.s['source_originals_unchanged']=True;self.persist()

    def checker_probe(self):
        self.setphase('functional')
        folder=SRC/'.validation';folder.mkdir()
        probe=folder/'checker_probe.cpp'
        probe.write_text('#include "TestCheck.h"\n#include <cstdio>\n#include <cstdlib>\n#include <cstddef>\n#include <cstring>\n#include <new>\nstd::size_t newCount = 0;\nstd::size_t mallocCount = 0;\nextern "C" void* __real_malloc(std::size_t);\nextern "C" void* __wrap_malloc(std::size_t bytes) {\n  ++mallocCount;\n  return __real_malloc(bytes);\n}\nvoid* operator new(std::size_t bytes) {\n  ++newCount;\n  if (auto memory = std::malloc(bytes)) return memory;\n  throw std::bad_alloc();\n}\nvoid operator delete(void* memory) noexcept { std::free(memory); }\nvoid operator delete(void* memory, std::size_t) noexcept { std::free(memory); }\nint main(int argc, char** argv) {\n  int counter = 0;\n  const auto beforeNew = newCount;\n  const auto beforeMalloc = mallocCount;\n  requireTestCondition(++counter == 1);\n  if (counter != 1 || newCount != beforeNew || mallocCount != beforeMalloc) return 91;\n  if (argc == 2 && std::strcmp(argv[1], "false") == 0) {\n    requireTestCondition(false);\n    return 92;\n  }\n  return 0;\n}\n')
        self.command(['/usr/bin/g++','-std=c++20','-O3','-DNDEBUG','-I',SRC/'tests',probe,'-Wl,--wrap=malloc','-o',folder/'checker_probe'],'checker-build')
        out=self.command([folder/'checker_probe'],'checker-true',cwd=folder)
        if out!='' or (R/'logs/checker-true.stderr').read_bytes():raise Reject('checker single-evaluation/allocation/output')
        self.command([folder/'checker_probe','false'],'checker-false',expect=-signal.SIGABRT,cwd=folder)
        stderr=(R/'logs/checker-false.stderr').read_text()
        false_line=next(i for i,line in enumerate(probe.read_text().splitlines(),1) if 'requireTestCondition(false)' in line)
        if not re.search(r'checker_probe\.cpp:'+str(false_line)+r'\b',stderr):raise Reject('checker callsite diagnostic')
        self.s['checker']={'true_side_effect_count':1,'allocation_delta':0,'malloc_delta':0,'checker_success_stdout_stderr':0,'false_signal':int(signal.SIGABRT),'release_flags':['-O3','-DNDEBUG'],'diagnostic_callsite':True}
        self.s['phase_seconds']['functional']=self.func_base+time.monotonic()-self.phase_start;self.persist()
    def versions(self):
        self.setphase('functional')
        for tool,args in [('cmake',['--version']),('/usr/bin/g++',['--version']),('python3',['--version']),('curl',['--version']),('bash',['--version']),('clang-format',['--version'])]:
            self.command([tool]+args,'version-'+pathlib.Path(tool).name)
        self.command(['clang-format','--dry-run','--Werror']+[SRC/p for p in ['tests/R6Callbacks_test.cpp','tests/HttpObservability_test.cpp','tests/ServerMetrics_test.cpp','tests/TestCheck.h']],'format-four')
        env=ENV.copy();env['LD_LIBRARY_PATH']=str(LIB)
        self.command([WRK,'--version'],'version-wrk',env=env,expect=1)
        self.s['phase_seconds']['functional']=self.func_base+time.monotonic()-self.phase_start;self.persist()
    def configure_build(self,name,kind,sanitizer=False):
        self.setphase('build-'+name);b=SRC/name
        if b.exists():raise Reject('generated tree must initially be absent')
        flags=['-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer','-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'] if sanitizer else ['-DCMAKE_CXX_FLAGS=']
        self.command(['cmake','-S',SRC,'-B',b,f'-DCMAKE_BUILD_TYPE={kind}','-DBUILD_TESTING=ON','-DCMAKE_EXPORT_COMPILE_COMMANDS=ON','-DCMAKE_CXX_COMPILER=/usr/bin/g++']+flags,'configure-'+name,180)
        argv=['cmake','--build',b,'-j2']
        if sanitizer:argv+=['--target']+SAN
        self.command(argv,'build-'+name,590)
        if not sanitizer:
            binary=b/'hp_http_server'
            deps=self.command(['ldd',binary],'ldd-'+name)
            paths=re.findall(r'(?:=>\s*)?(/[^ ]+)\s+\(',deps)
            self.s.setdefault('binaries',{})[name]={'binary_sha256':sha(binary),'runtime_libraries':{x:sha(x) for x in paths},'compile_commands_sha256':sha(b/'compile_commands.json'),'cmake_cache_sha256':sha(b/'CMakeCache.txt')}
        self.s['phase_seconds'][self.phase]=time.monotonic()-self.phase_start
        self.source_check();self.persist()
    def functional_phase(self,callback):
        self.setphase('functional');callback();self.source_check();self.s['phase_seconds']['functional']=self.func_base+time.monotonic()-self.phase_start;self.persist()
    def tests(self,name):
        def check():
            b=SRC/name
            listing=self.command(['ctest','--test-dir',b,'--show-only=json-v1'],'registered-'+name)
            rows=json.loads(listing)['tests'];save(R/'manifest'/f'{name}-ctest.json',rows)
            if len(rows)!=14 or len({r['name'] for r in rows})!=14:raise Reject('unexpected CTest inventory')
            text=self.command(['ctest','--test-dir',b,'--output-on-failure','-j1'],'ctest-'+name,590)
            if '100% tests passed, 0 tests failed out of 14' not in text:raise Reject('CTest completion missing')
        self.functional_phase(check)
    def sanitizer(self):
        def check():
            env=ENV.copy();env.update(ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
            text=self.command(['ctest','--test-dir',SRC/'build-asan','-R','^('+'|'.join(SAN)+')$','--output-on-failure','-j1'],'sanitizer-five',590,env)
            if '100% tests passed, 0 tests failed out of 5' not in text:raise Reject('sanitizer completion missing')
        self.functional_phase(check)
    def server(self,binary,root,workers,label,requested_port=0):
        p,row=self.spawn([binary,'--port',str(requested_port),'--root',root,'--threads',str(workers),'--idle-timeout-ms','30000','--keep-alive-timeout-ms','15000','--shutdown-timeout-ms','5000','--metrics-on-exit'],label)
        end=time.monotonic()+5
        while time.monotonic()<end:
            if p.poll() is not None:raise Reject('server startup exit')
            text=pathlib.Path(row['out']).read_text(errors='replace');m=re.search(r'listening on port (\d+)\.',text)
            if m:
                port=int(m[1]);inodes={}
                for fd in pathlib.Path('/proc',str(p.pid),'fd').iterdir():
                    try:
                        link=fd.readlink();match=re.fullmatch(r'socket:\[(\d+)\]',str(link))
                        if match:inodes[match[1]]=str(fd)
                    except FileNotFoundError:pass
                found=[]
                for line in pathlib.Path('/proc/net/tcp').read_text().splitlines()[1:]:
                    f=line.split()
                    if int(f[1].split(':')[1],16)==port and f[3]=='0A' and f[9] in inodes:found.append({'local':f[1],'inode':f[9],'fd':inodes[f[9]]})
                if len(found)!=1:raise Reject('listener ownership')
                row['listener']=found;row['port']=port;self.persist();self.take_scan(force=True);return p,row,port
            self.guard(end);time.sleep(.05)
        raise Reject('server port deadline')
    def ready_wait(self,sk,writing,deadline,timeout_error=True):
        while True:
            self.guard();now=time.monotonic()
            if now>=deadline:
                if timeout_error:raise Reject('socket operation deadline')
                readable,writable,errors=select.select([] if writing else [sk],[sk] if writing else [],[sk],0)
                return bool(errors or (writable if writing else readable))
            wait=min(.1,deadline-now,max(.001,self.next_tick-now))
            readable,writable,errors=select.select([] if writing else [sk],[sk] if writing else [],[sk],wait)
            self.guard()
            if errors:raise Reject('socket exceptional readiness')
            if writable if writing else readable:return True
    def connect(self,port):
        sk=socket.socket(socket.AF_INET,socket.SOCK_STREAM);sk.setblocking(False);end=time.monotonic()+2
        try:
            err=sk.connect_ex(('127.0.0.1',port))
            if err not in (0,errno.EINPROGRESS,errno.EWOULDBLOCK,errno.EALREADY):raise Reject('connect errno '+str(err))
            if err:self.ready_wait(sk,True,end);err=sk.getsockopt(socket.SOL_SOCKET,socket.SO_ERROR)
            if err:raise Reject('connect SO_ERROR '+str(err))
            if time.monotonic()>=end:raise Reject('connect deadline')
            return sk
        except Exception:sk.close();raise
    def send_all(self,sk,data):
        end=time.monotonic()+2;sent=0
        while sent<len(data):
            self.ready_wait(sk,True,end)
            try:count=sk.send(data[sent:])
            except BlockingIOError:continue
            if count<=0:raise Reject('send EOF')
            sent+=count;self.event('send_progress',sent=sent,total=len(data),deadline=end)
        if time.monotonic()>=end:raise Reject('send deadline')
    def receive(self,sk,index,part,cumulative):
        end=time.monotonic()+2;self.event('recv_begin',index=index,part=part,cumulative=cumulative,deadline=end)
        while True:
            self.ready_wait(sk,False,end)
            try:chunk=sk.recv(65536)
            except BlockingIOError:continue
            self.event('recv_end',index=index,part=part,cumulative=cumulative+len(chunk),bytes=len(chunk))
            if time.monotonic()>=end:raise Reject('recv deadline')
            return chunk
    def audit(self,port,body,path='/payload.bin'):
        sk=self.connect(port);notes=[];self.event('audit_connected',port=port,local=sk.getsockname(),path=path)
        try:
            for index in range(3):
                self.guard();self.event('send_begin',index=index,path=path);self.send_all(sk,f'GET {path} HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n'.encode());self.event('send_end',index=index);buf=b''
                while b'\r\n\r\n' not in buf:
                    chunk=self.receive(sk,index,'header',len(buf))
                    if not chunk:raise Reject('header EOF request '+str(index))
                    buf+=chunk
                head,buf=buf.split(b'\r\n\r\n',1);lines=head.split(b'\r\n');headers={}
                if lines[0]!=b'HTTP/1.1 200 OK':raise Reject('audit status')
                for line in lines[1:]:
                    k,v=line.split(b':',1);key=k.lower()
                    if key in headers:raise Reject('duplicate header')
                    headers[key]=v.strip()
                if headers.get(b'connection')!=b'keep-alive' or int(headers[b'content-length'])!=len(body):raise Reject('audit length/connection')
                while len(buf)<len(body):
                    chunk=self.receive(sk,index,'body',len(buf))
                    if not chunk:raise Reject('body EOF request '+str(index))
                    buf+=chunk
                if buf!=body:raise Reject('audit body/tail')
                self.event('tail_begin',index=index);tail_end=time.monotonic()+.02
                if self.ready_wait(sk,False,tail_end,timeout_error=False):raise Reject('unsolicited bytes or premature close')
                self.event('tail_end',index=index);note={'status':200,'bytes':len(buf),'sha256':hashlib.sha256(buf).hexdigest(),'same_socket_request':index+1,'tail_bytes':0};notes.append(note);self.event('audit_request_complete',index=index,observation=note)
        except Exception as exc:self.event('audit_failure',request_index=index if 'index' in locals() else None,error=repr(exc));raise
        finally:sk.close();self.event('audit_socket_closed')
        return notes

    def metrics(self,row):
        text=pathlib.Path(row['out']).read_text()
        if text.count('HP_METRICS_BEGIN')!=1 or text.count('HP_METRICS_END')!=1:raise Reject('metrics cardinality')
        block=text.split('HP_METRICS_BEGIN\n')[1].split('HP_METRICS_END')[0];d={}
        for line in block.splitlines():
            k,v=line.split();d[k]=int(v)
        for k in ['connections_active','logger_pending']:
            if d[k]!=0:raise Reject('exit metric '+k)
        if d['requests_started_total']!=d['responses_completed_total']+d['requests_aborted_total'] or d['latency_count']!=d['requests_started_total']:raise Reject('request accounting')
        return d
    def smoke(self,name):
        def check():
            for workers in [0,2]:
                env=ENV.copy();env.update(HP_S3_TEST_TMP_ROOT=str(SRC/'.validation/smoke'),HP_HTTP_TEST_THREADS=str(workers))
                self.command(['bash',SRC/'tests/http_smoke_test.sh',SRC/name/'hp_http_server'],f'smoke-{name}-{workers}',60,env)
            binary=SRC/name/'hp_http_server';self.command([binary,'--help'],'help-'+name)
            self.command([binary],'missing-cli-'+name,expect=2)
            requested=8080
            with socket.socket(socket.AF_INET,socket.SOCK_STREAM) as probe:
                try:probe.bind(('0.0.0.0',8080));port_reason='README port available'
                except OSError as exc:
                    if exc.errno!=98:raise
                    requested=0;port_reason='README 8080 occupied; documented kernel port allocation'
            p,row,port=self.server(binary,SRC/'www',2,'readme-'+name,requested)
            row['readme_requested_port']=requested;row['readme_port_reason']=port_reason
            try:
                body=(SRC/'www/index.html').read_bytes();notes=self.audit(port,body,'/')
                for method,path,expected in [('GET','/missing-V1-S3',404),('POST','/',405)]:
                    sk=self.connect(port)
                    try:
                        self.send_all(sk,f'{method} {path} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nContent-Length: 0\r\n\r\n'.encode());raw=b''
                        while True:
                            part=self.receive(sk,0,'readme-error',len(raw))
                            if not part:break
                            raw+=part
                        if int(raw.split(b' ',2)[1])!=expected:raise Reject('README request status')
                    finally:sk.close()
                self.stop(p,row,signal.SIGINT if name=='build' else signal.SIGTERM);row['metrics']=self.metrics(row);row['readme_audit']=notes;self.persist()
            finally:
                if p.pid in self.live:self.stop(p,row)
        self.functional_phase(check)
    def r004_governance(self):
        self.setphase('functional')
        self.command([sys.executable,R/'tmp/schedule_probe.py',str(pathlib.Path(__file__))],'r004-schedule-probe',15)
        checks=[]
        for case in ('success','failure','late-failure'):
            folder=R/'fixtures'/('terminal-'+case)
            for name in ('tmp','logs','manifest','source'): (folder/name).mkdir(parents=True,exist_ok=True)
            self.guard();shutil.copyfile(pathlib.Path(__file__),folder/'tmp/review_run.py');self.guard()
            fixture={'commands':[],'children':[],'samples':[],'capture_paths':{},'peak_total':0,'peak_log':0,'status':'running','phase_seconds':{'functional':0},'candidate':COMMIT,'monitor_scans':[],'active_windows':[],'start':self.s['start'],'started_utc':self.s['started_utc']}
            save(folder/'manifest/state.json',fixture);self.register_capture(folder/'logs/trace.jsonl','r004-terminal-fixture-trace')
            for stream in ('stdout','stderr'):self.register_capture(folder/'logs'/('terminal-fixture-owned.'+stream),'r004-terminal-fixture-process')
            self.guard()
            raw=self.command([sys.executable,folder/'tmp/review_run.py','terminal-probe-'+case], 'r004-terminal-'+case,20,expect=0 if case=='success' else 1)
            rows=[json.loads(line) for line in raw.splitlines() if line.startswith('{')]
            if len(rows)!=1:raise Reject('terminal fixture packet cardinality')
            packet=rows[0];scan=packet['terminal_snapshot'];end=packet['active_end_monotonic']
            if not packet['terminal_complete'] or end is None or packet['owned_left']!=0:raise Reject('terminal fixture incomplete/reap')
            if not packet['probe_last_owned_write']<=scan['start']<=scan['complete']<=end:raise Reject('terminal fixture last-write order')
            if packet['role_elapsed_s']!=end-self.s['start'] or max(scan['duration'],scan['start_interval'],scan['complete_interval'],end-scan['start'],end-scan['complete'])>1:raise Reject('terminal fixture time evidence')
            if case=='success' and (packet['status']=='stopped' or packet['first_error'] or packet['governance_error']):raise Reject('success fixture false fail')
            if case=='failure' and (packet['status']!='stopped' or packet['first_error']!="Reject('intentional-fixture-first-error')" or packet['governance_error']!='intentional fixture latch'):raise Reject('failed fixture primary changed')
            if case=='late-failure' and (packet['status']!='stopped' or packet['first_error']!="Reject('active scan start/complete/duration >1s')" or packet['governance_error']!='active scan start/complete/duration >1s'):raise Reject('late terminal failure false pass')
            checks.append({'case':case,'packet':packet,'expected_exit':0 if case=='success' else 1})
        self.s['r004_governance']=checks;self.s['phase_seconds']['functional']=self.func_base+time.monotonic()-self.phase_start;self.persist()

    def local_admission(self):
        self.setphase('functional');checks=[]
        good={'schema':2,'duration_us':1000000,'requests':10,'bytes':100,'errors':dict.fromkeys(['connect','read','write','status','timeout'],0),'latency_us':{'mean':2,'p50':1,'p95':2,'p99':3,'max':4},'latency_distribution':'wrk_corrected','population_status':'not_collected','corrected_population':None,'nonzero_bins':None,'correction_interval_us':1000}
        fmt=lambda d:'MATRIX_SUMMARY '+json.dumps(d)
        assert parse_summary(fmt(good))['qps']==10;checks.append('valid-summary')
        bads=['',fmt(good)+'\n'+fmt(good)]
        for key in ['requests','errors','latency_us']:
            d=json.loads(json.dumps(good));del d[key];bads.append(fmt(d))
        d=json.loads(json.dumps(good));d['errors']['timeout']=1;bads.append(fmt(d))
        for key,value in [('requests',0),('duration_us',0),('bytes',-1),('correction_interval_us',float('nan'))]:
            d=json.loads(json.dumps(good));d[key]=value;bads.append(fmt(d))
        bads.append(fmt(good).replace('"schema": 2','"schema": 2, "schema": 2'))
        d=json.loads(json.dumps(good));d['latency_us']['p99']=5;bads.append(fmt(d))
        for bad in bads:
            try:parse_summary(bad)
            except (Reject,KeyError,ValueError,TypeError):checks.append('rejected')
            else:raise Reject('summary negative accepted')
        # Original policy compiled by AST only; compare both functions on the same real,
        # quiescent new-role tree. Explicit disposable inputs create no CMake cache/build
        # outputs; candidate generated-root absence is checked before and after cleanup.
        original_text=(REPO/'.cache/v1.0-s3/builder/rework-001/tmp/validate.py').read_text()
        node=next(x for x in ast.parse(original_text).body if isinstance(x,ast.FunctionDef) and x.name=='scan')
        fixture_source=SRC
        if any((SRC/g).exists() for g in GEN):raise Reject('governance fixture roots must initially be absent')
        paths=[fixture_source/'build/test-tmp/synthetic-case/oversize.stdout',fixture_source/'build/test-tmp/synthetic-case/sample-01/server.stderr',fixture_source/'build/test-tmp/synthetic-case/real.stdout',fixture_source/'build/Testing/run.log',fixture_source/'build/TestingX/run.log',R/'tmp/same-name/real.stdout',R/'logs/fixture-capture/real.stdout']
        for path in paths:path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(b'input')
        with paths[0].open('r+b') as handle:handle.truncate(513*1024**2)
        self.register_capture(paths[-1],'fixture registered capture')
        (fixture_source/'build/file-link').symlink_to(paths[-1]);(fixture_source/'build/directory-link').symlink_to(paths[0].parent,target_is_directory=True)
        saved_source,saved_generated=self.scan_source,self.generated;self.scan_source=fixture_source;self.generated=[fixture_source/x for x in GEN[:3]]
        try:
            ns={'ROOT':R,'SOURCE':fixture_source,'GENERATED':self.generated+[fixture_source/'.validation'],'STATE':{'captures':dict(self.s['capture_paths'])}}
            exec(compile(ast.Module(body=[node],type_ignores=[]),'<frozen-reference-scan>','exec'),ns)
            self.take_scan(force=True);legacy_begin=time.monotonic();legacy=ns['scan']();legacy_end=time.monotonic();actual=self.scan();self.guard()
            if legacy[:2]!=actual or legacy[2]!=self.s['dynamic_classifications']:raise Reject('real-tree original classification mismatch')
            if self.s['dynamic_classifications'][str(paths[0].relative_to(R))]!='frozen-test-input' or str(paths[5]) in self.s['capture_paths']:raise Reject('fixture/full-path classification')
            checks.append({'exact_original_scan_equal':True,'legacy_scan_s':legacy_end-legacy_begin,'sparse_logical_bytes':paths[0].stat().st_size,'symlinks_and_directory_boundary':True})
        finally:
            self.scan_source,self.generated=saved_source,saved_generated;shutil.rmtree(SRC/'build');shutil.rmtree(R/'tmp/same-name');shutil.rmtree(R/'logs/fixture-capture');self.s['capture_paths'].pop(str(paths[-1]),None)
        if any((SRC/g).exists() for g in GEN):raise Reject('fixture cleanup did not restore absent generated roots')
        self.take_scan(force=True)
        # Independent partial-send/EAGAIN and short-read probes use fake readiness,
        # but the actual socket-operation methods and a single recorded deadline.
        class Fake:
            def __init__(self):self.output=b'';self.calls=0
            def send(self,data):
                self.calls+=1
                if self.calls==1:raise BlockingIOError()
                count=min(2,len(data));self.output+=data[:count];return count
            def recv(self,count):
                self.calls+=1
                if self.calls<3:raise BlockingIOError()
                return b'xy'
        original_wait=self.ready_wait;deadlines=[]
        def fake_wait(sk,writing,deadline,timeout_error=True):
            self.guard();deadlines.append(deadline)
            if time.monotonic()>=deadline:raise Reject('socket operation deadline')
            return True
        try:
            self.ready_wait=fake_wait;fake=Fake();self.send_all(fake,b'abcdef')
            if fake.output!=b'abcdef' or len(set(deadlines))!=1:raise Reject('send suffix/deadline reset')
            deadlines.clear();fake=Fake();chunk=self.receive(fake,0,'synthetic',0)
            if chunk!=b'xy' or len(set(deadlines))!=1:raise Reject('recv EAGAIN/deadline reset')
            checks.append('send-suffix-short-read-EAGAIN-fixed-deadline')
            class NeverWrite:
                def send(self,data):raise BlockingIOError()
            expiry_calls=[]
            def paced_busy(sk,writing,deadline,timeout_error=True):
                self.guard();expiry_calls.append(deadline)
                if time.monotonic()>=deadline:raise Reject('socket operation deadline')
                time.sleep(.02);return True
            self.ready_wait=paced_busy;began=time.monotonic()
            try:self.send_all(NeverWrite(),b'x')
            except Reject as exc:
                if 'socket operation deadline' not in str(exc) or len(set(expiry_calls))!=1 or not 2<=time.monotonic()-began<3:raise
                checks.append('EAGAIN-retries-expire-at-original-2s-deadline')
            else:raise Reject('socket deadline negative accepted')
        finally:self.ready_wait=original_wait
        # Real self-owned socketpair traverses select/send/recv; wrappers inject
        # short writes/reads and EAGAIN while keeping actual kernel byte transfer.
        left,right=socket.socketpair();left.setblocking(False);right.setblocking(False)
        try:
            class PartialWriter:
                def __init__(self):self.calls=0;self.suffixes=[]
                def fileno(self):return left.fileno()
                def send(self,data):
                    self.calls+=1;self.suffixes.append(data)
                    if self.calls==2:raise BlockingIOError()
                    return left.send(data[:2])
            writer=PartialWriter();self.send_all(writer,b'abcdef')
            if writer.suffixes!=[b'abcdef',b'cdef',b'cdef',b'ef']:raise Reject('real partial-send suffix')
            class PartialReader:
                def __init__(self):self.calls=0
                def fileno(self):return right.fileno()
                def recv(self,count):
                    self.calls+=1
                    if self.calls==1:raise BlockingIOError()
                    return right.recv(min(2,count))
            reader=PartialReader();received=b''.join(self.receive(reader,i,'real-short-read',i*2) for i in range(3))
            if received!=b'abcdef':raise Reject('real short-read transfer')
            if self.ready_wait(right,False,time.monotonic()+.02,timeout_error=False):raise Reject('real tail quiet window')
            began=time.monotonic()
            try:self.receive(right,0,'real-timeout',0)
            except Reject as exc:
                if 'socket operation deadline' not in str(exc) or not 2<=time.monotonic()-began<3:raise
            else:raise Reject('real empty socket failed to expire')
            checks.append('real-socketpair-select-slices-transfer-EAGAIN-2s-expiry-tail20ms')
        finally:left.close();right.close()
        # Deadline/limit failures use the real command boundary and real captures.
        # Real harmless child uses actual command boundary and finally cleanup.
        p,row=self.spawn(['/usr/bin/sleep','2'],'admission-deadline')
        try:
            self.wait(p,row,.05)
        except Reject as e:
            if 'command deadline' not in str(e):raise
            self.stop_sleep(p,row);checks.append('deadline-finally-reap')
        else:raise Reject('deadline did not fire')
        # Same actual monitor with a deliberately small synthetic limit; real limit unchanged.
        p,row=self.spawn([sys.executable,'-c',"import sys,time;sys.stdout.write('x'*8192);sys.stdout.flush();time.sleep(2)"],'admission-output-limit')
        try:
            end=time.monotonic()+1
            while pathlib.Path(row['out']).stat().st_size<8192:
                self.guard(end);time.sleep(.01)
            self.take_scan(force=True);current_total,current_log=self.s['final_total'],self.s['final_log']
            try:self.guard(log_limit=current_log-1)
            except Reject as e:
                if 'output limit' not in str(e):raise
                checks.append('output-limit-real-child-and-finally')
            else:raise Reject('output limit failed to reject')
        finally:self.stop_sleep(p,row)
        p,row=self.spawn([sys.executable,'-c','import signal,time; signal.signal(signal.SIGTERM,lambda *_:None); print("armed",flush=True); time.sleep(.35)'],'admission-term-sliced-wait')
        try:
            end=time.monotonic()+2
            while 'armed' not in pathlib.Path(row['out']).read_text():self.guard(end);time.sleep(.01)
            self.stop(p,row);checks.append('real-TERM-ignored-briefly-then-zero-exit-sliced-poll')
        finally:
            if p.pid in self.live:self.stop_sleep(p,row)
        self.s['local_admission']=checks;self.s['phase_seconds']['functional']=self.func_base+time.monotonic()-self.phase_start;self.persist()
    def stop_sleep(self,p,row):
        if pidstart(p.pid)!=row['starttime']:raise Reject('synthetic identity')
        p.terminate();end=time.monotonic()+2
        while p.poll() is None and time.monotonic()<end:self.guard(cleanup=True);time.sleep(.01)
        if p.poll() is None:p.kill();p.wait();raise Reject('synthetic forced')
        p.wait();row['rc']=p.returncode;row['reaped']=True;row['finished']=time.monotonic();_,_,oh,eh=self.live.pop(p.pid);oh.close();eh.close();self.persist();self.take_scan(force=True)

    def sample(self,scene,label,warm,measure):
        size,workers,threads,connections=SCENES[scene];folder=R/'performance'/label
        if sha(SRC/'build-release/hp_http_server',self.guard)!=self.s['binaries']['build-release']['binary_sha256']:raise Reject('Release binary identity drift')
        if sha(R/'candidate.patch',self.guard)!=json.loads((R/'manifest/identity.json').read_text())['patch_sha256']:raise Reject('patch identity drift')
        if folder.exists():raise Reject('sample overwrite')
        folder.mkdir();payload=folder/'payload';payload.mkdir();body=b'0123456789abcdef'*(size//16);(payload/'payload.bin').write_bytes(body)
        result={'id':label,'scene':scene,'status':'pending','body_bytes':size,'workers':workers,'threads':threads,'connections':connections,'warm_seconds':warm,'measure_seconds':measure,'payload_sha256':hashlib.sha256(body).hexdigest()};save(folder/'result.json',result)
        if label=='admission':self.s['performance_admission']=result
        else:
            self.s['samples'][int(label.split('-')[0])-1]=result
            self.s.setdefault('started_formal',[]).append(label)
        self.persist()
        p=row=None
        try:
            p,row,port=self.server(SRC/'build-release/hp_http_server',payload,workers,label+'-server');result['pre_audit']=self.audit(port,body)
            env=ENV.copy();env.update(LD_LIBRARY_PATH=str(LIB),HP_MATRIX_MODE='keepalive',HP_MATRIX_CONNECTIONS=str(connections))
            for part,duration in [('warm',warm),('measure',measure)]:
                raw=self.command([WRK,'-t',str(threads),'-c',str(connections),'-d',str(duration)+'s','--timeout','2s','--latency','-s',LUA,f'http://127.0.0.1:{port}/payload.bin'],label+'-'+part,duration+8,env)
                result[part]=parse_summary(raw,size,connections)
            result['post_audit']=self.audit(port,body);self.stop(p,row);result['metrics']=self.metrics(row);result['server']=row;result['status']='valid'
        except Exception as e:result['error']=repr(e);result['status']='invalid';raise
        finally:
            if p is not None and p.pid in self.live:
                try:self.stop(p,row)
                except Exception as e:result['cleanup_error']=repr(e);result['status']='invalid'
            if payload.exists():shutil.rmtree(payload)
            result['cleanup_reaped']=row is not None and row.get('reaped',False);save(folder/'result.json',result)
            if label=='admission':self.s['performance_admission']=result
            else:self.s['samples'][int(label.split('-')[0])-1]=result
            self.persist()
        if result['status']!='valid':raise Reject('sample cleanup invalid')
    def performance(self):
        self.setphase('performance');self.sample('M2','admission',1,1)
        for i,scene in enumerate(ORDER):self.sample(scene,f'{i+1:02d}-{scene}',2,10)
        self.s['phase_seconds']['performance']=time.monotonic()-self.phase_start
        aggregates={}
        for scene in SCENES:
            rows=[x['measure'] for x in self.s['samples'] if x['scene']==scene]
            aggregates[scene]={}
            for key,vals in [('qps',[x['qps'] for x in rows]),('received_mib_s',[x['received_mib_s'] for x in rows]),('corrected_p99_ms',[x['latency_us']['p99']/1000 for x in rows])]:aggregates[scene][key]={'median':statistics.median(vals),'min':min(vals),'max':max(vals),'round_values':vals}
        self.s['aggregates']=aggregates;self.source_check();self.readonly_final_check();self.s['status']='dynamic-complete';self.persist()
def prepare():
    originals={}
    fixed=json.loads((R/'manifest/patch-identity.json').read_text())
    with tarfile.open(R/'source.tar') as archive:
        for m in archive.getmembers():
            if m.isfile():
                with archive.extractfile(m) as f:expected=hashlib.sha256(f.read()).hexdigest()
                actual=sha(SRC/m.name)
                if m.name in fixed['changed_files']:
                    if actual!=fixed['candidate_sha256'][m.name]:raise Reject('patch identity mismatch')
                elif actual!=expected:raise Reject('archive extraction drift outside patch')
                originals[m.name]=actual
    originals['tests/TestCheck.h']=sha(SRC/'tests/TestCheck.h')
    if originals['tests/TestCheck.h']!=fixed['candidate_sha256']['tests/TestCheck.h']:raise Reject('checker identity mismatch')
    save(R/'manifest/original-files.json',originals)
    for g in GEN:
        if (SRC/g).exists():raise Reject('generated tree exists before admission')
    if sha(WRK)!='b10e53769443c2bf3be2cdedec8ef6571aa5bfd1494796b247f3f9296e3af71d' or sha(LUA)!='0706c8defa1a759eda90185ca5c9f02d755bb3c5a7248367d5f0c3618111340e':raise Reject('readonly tool drift')
    libpaths=list((REPO/'.cache/v0.5-s4/tools/root/usr').rglob('*.so*'))
    save(R/'manifest/identity.json',{'commit':COMMIT,'archive_sha256':sha(R/'source.tar'),'original_count':len(originals),'patch_sha256':fixed['patch_sha256'],'candidate_files_sha256':fixed['candidate_sha256'],'wrk_sha256':sha(WRK),'lua_sha256':sha(LUA),'library_files':{str(p):sha(p) for p in libpaths if p.is_file()},'generated_trees':[str(SRC/g) for g in GEN],'env':{k:ENV[k] for k in ['TMPDIR','TMP','TEMP','XDG_CACHE_HOME','PYTHONDONTWRITEBYTECODE','NO_PROXY']},'commands':{x:shutil.which(x) for x in ['cmake','ctest','g++','python3','bash','curl']},'host_kernel':pathlib.Path('/proc/sys/kernel/osrelease').read_text().strip(),'cpu_model':next((x.split(':',1)[1].strip() for x in pathlib.Path('/proc/cpuinfo').read_text().splitlines() if x.startswith('model name')),'unknown'),'script_sha256':sha(pathlib.Path(__file__)),'publication_outputs':[str(p) for p in PUBLIC],'publication_producer':'owned reviewer packaging subprocess'})
    oldclock=REPO/'.cache/v1.0-s3/leader/r003-old-clock-sealed.json'
    if not oldclock.exists():raise Reject('sealed prior-clock record missing')
    save(R/'manifest/sealed-old-clock.json',json.loads(oldclock.read_text()))
    original_text=(REPO/'.cache/v1.0-s3/builder/rework-001/tmp/validate.py').read_text()
    node=next(x for x in ast.parse(original_text).body if isinstance(x,ast.FunctionDef) and x.name=='scan')
    (R/'manifest/reference-scan.txt').write_text(ast.get_source_segment(original_text,node)+'\n')
    print('static preparation: originals and readonly identities verified; dynamic not started')
if __name__=='__main__':
    mode=sys.argv[1]
    if mode=='prepare':prepare();sys.exit(0)
    d=Driver()
    def interrupted(number,frame):
        raise Reject('external signal '+str(number))
    signal.signal(signal.SIGINT,interrupted)
    signal.signal(signal.SIGTERM,interrupted)
    if mode.startswith('terminal-probe-'):
        PROBE_LAST_WRITE=0
        original_save=save;original_event=d.event
        def probe_save(path,obj):
            global PROBE_LAST_WRITE
            original_save(path,obj);PROBE_LAST_WRITE=time.monotonic()
        def probe_event(kind,**fields):
            global PROBE_LAST_WRITE
            writing=d.trace is not None
            original_event(kind,**fields)
            if writing:PROBE_LAST_WRITE=time.monotonic()
        save=probe_save;d.event=probe_event
    try:
        if mode.startswith('terminal-probe-'):
            d.setphase('functional');d.command([sys.executable,'-c','import time;time.sleep(.02)'],'terminal-fixture-owned',5)
            if mode=='terminal-probe-failure':
                d.governance_error='intentional fixture latch';d.s['governance_error']=d.governance_error
                raise Reject('intentional-fixture-first-error')
            if mode=='terminal-probe-late-failure':
                original_scan=d.scan;injected=[False]
                def late_scan():
                    result=original_scan()
                    if d.trace is None and not injected[0]:injected[0]=True;time.sleep(1.02)
                    return result
                d.scan=late_scan
        elif mode=='governance':
            d.admit();d.r004_governance();d.local_admission();d.versions();d.checker_probe()
        elif mode=='build':d.configure_build('build','Debug');d.configure_build('build-release','Release')
        elif mode=='functional':d.tests('build');d.tests('build-release');d.smoke('build');d.smoke('build-release')
        elif mode=='asan-build':d.configure_build('build-asan','Debug',True)
        elif mode=='asan-tests':d.sanitizer()
        elif mode=='performance':d.performance();d.cleanup()
        elif mode=='package':
            d.setphase('publication');d.s['publication_outputs']=[str(x) for x in PUBLIC];d.s['publication_producer']='owned reviewer packaging subprocess';d.take_scan(force=True)
            d.command([sys.executable,R/'tmp/package.py'],'publication-package',120,cwd=REPO);d.s['packaged']=True;d.persist()
        else:raise Reject('unknown stage')
        d.park()
    except Exception as e:
        d.s['status']='stopped';d.s.setdefault('error',repr(e))
        if 'start' in d.s:
            try:d.cleanup()
            except Exception as cleanup_error:d.s['cleanup_error']=repr(cleanup_error)
            try:d.park(failed=True)
            except Exception as final_error:d.s['finalization_error']=repr(final_error)
        else:d.persist()
        print('STOP:',d.s['error'])
    active_end=d.s['active_windows'][-1].get('actual_end_monotonic') if d.s.get('active_windows') else None
    print(json.dumps({'stage':mode,'status':d.s['status'],'terminal_complete':d.s.get('terminal_complete',False),'first_error':d.s.get('error'),'cleanup_error':d.s.get('cleanup_error'),'finalization_error':d.s.get('finalization_error'),'governance_error':d.s.get('governance_error'),'terminal_snapshot':d.last_scan,'active_end_monotonic':active_end,'role_elapsed_s':(active_end if active_end is not None else time.monotonic())-d.s['start'] if 'start' in d.s else None,'owned_left':len(d.live)+len(d.s.get('remaining_children',[])),'probe_last_owned_write':globals().get('PROBE_LAST_WRITE')},separators=(',',':')))
    if d.s['status']=='stopped':sys.exit(1)
