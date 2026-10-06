"""Approved R001: independent diagnostic, never formal acceptance."""
import argparse,json,math,pathlib,re,time,subprocess,hashlib,io,tarfile
import budget,identity,model,supervision
from identity import legacy

CLOCKS={'realtime':time.CLOCK_REALTIME,'monotonic':time.CLOCK_MONOTONIC}
if hasattr(time,'CLOCK_BOOTTIME'):
    CLOCKS['boottime']=time.CLOCK_BOOTTIME

def clocks():
    result={}
    for k,v in CLOCKS.items():
        try: result[k]=time.clock_gettime(v)
        except (OSError,ValueError): result[k]=None
    result.update(utc=legacy.utc(),process_cpu=time.process_time())
    return result

def assess(first,previous,current):
    elapsed=[]; flags=[]
    for k in ('realtime','monotonic','boottime'):
        values=[r.get(k) for r in (first,previous,current)]
        if any(type(v) not in (float,int) or not math.isfinite(v) for v in values):
            flags.append(k+': unavailable/invalid'); continue
        elapsed.append(max(0,values[2]-values[0]))
        if values[2]<values[1]: flags.append(k+': backward')
        if values[2]-values[1]>.5: flags.append(k+': sampling pause/gap')
    legacy.demand(bool(elapsed),'no usable clock')
    if max(elapsed)-min(elapsed)>.5: flags.append('clock disagreement')
    return max(elapsed),flags

def terminal_status(record, flags):
    # The last sample may discover a pause after identity verification. It must
    # invalidate the outcome even when all earlier phases were observed.
    if flags:
        record['status']='invalid'
    return record


class Observer:
    def __init__(self,path):
        self.first=self.previous=clocks(); self.maximum=0; self.flags=set(); self.sequence=0
        self.stream=path.open('w')
    def tick(self,phase,owned=()):
        current=clocks(); elapsed,flags=assess(self.first,self.previous,current)
        self.maximum=max(self.maximum,elapsed); self.flags.update(flags)
        states={}
        for child in owned:
            try: states[str(child.process.pid)]=legacy.process_info(child.process.pid)
            except FileNotFoundError: states[str(child.process.pid)]={'state':'unknown','starttime':child.identity['starttime']}
        current.update(sequence=self.sequence,phase=phase,elapsed_max=self.maximum,flags=flags,processes=states)
        self.stream.write(json.dumps(current)+'\n'); self.stream.flush()
        self.previous=current; self.sequence+=1
        return self.maximum

def cleanup(child,observer,root,phase):
    # Cleanup cannot be prevented by a sampling/log failure. PID/starttime is
    # rechecked before every signal; the retained Popen is always waited.
    forced=False; errors=[]; first=clocks()
    try:
        if child.alive() and child.matches(): child.process.terminate()
        while child.alive():
            if observer is not None:
                try: observer.tick(phase,[child])
                except BaseException as error: errors.append(str(error)); break
            try: elapsed=assess(first,first,clocks())[0]
            except BaseException as error: errors.append(str(error)); break
            if elapsed>=7: break
            time.sleep(.05)
    except BaseException as error:
        errors.append(type(error).__name__+': '+str(error))
    finally:
        if child.alive():
            forced=True
            if child.matches(): child.process.kill()
        reaped=True
        try: child.process.wait(timeout=1)
        except subprocess.TimeoutExpired: reaped=False
        child.stdout.close(); child.stderr.close()
    return dict(pid=child.process.pid,starttime=child.identity['starttime'],forced=forced,reaped=reaped,returncode=child.process.returncode,errors=errors)

def static(wrk):
    package=identity.REPO/'.cache/v0.5-s4/tools/packages/wrk.deb'
    archive=subprocess.check_output(['dpkg-deb','--fsys-tarfile',str(package)])
    with tarfile.open(fileobj=io.BytesIO(archive)) as contents:
        binary=contents.extractfile('./usr/bin/wrk').read()
    package_binary_hash=hashlib.sha256(binary).hexdigest()
    legacy.demand(package_binary_hash==legacy.sha(wrk),'wrk package payload identity mismatch')
    return dict(package_binary_sha256=package_binary_hash,package_binary_matches=True,kernel=pathlib.Path('/proc/version').read_text().strip(),
        clocksource=pathlib.Path('/sys/devices/system/clocksource/clocksource0/current_clocksource').read_text().strip(),
        wrk=identity.stable_tool(legacy.validate_tool(wrk)),
        package_metadata=subprocess.check_output(['dpkg-deb','-f',str(package),'Package','Version'],text=True),
        package_sha256=legacy.sha(package),
        clock_symbol_clues=[line.strip() for line in subprocess.check_output(['readelf','--dyn-syms','--wide',str(wrk)],text=True).splitlines() if any(k in line for k in ('gettimeofday','clock_gettime','time@'))],
        wrk_clock_source='unknown: corresponding local source unavailable; no network',
        clock_interfaces={k:time.get_clock_info(k).__dict__ for k in ('time','monotonic','process_time')},
        observer='clock_gettime REALTIME/MONOTONIC/BOOTTIME; process_time; 100ms target, gap >500ms unknown/invalid; overhead included')

def diagnose(args):
    root=identity.role_root(args.role)
    data=budget.load(root/'ledger.json')
    legacy.demand(not any(e.get('kind')=='diagnostic-clock' for e in data['entries'].values()),'one clock diagnostic per role')
    supervision.resources(root)
    reservation=budget.Reservation(root,args.output,'diagnostic-clock',240)
    output=reservation.output; observer=None; owned=[]
    record=dict(run_id=reservation.id,kind='diagnostic-clock',status='invalid',started_utc=legacy.utc(),
        prior_dynamic_seconds=reservation.previous,samples=[],performance_acceptance='NOT_APPLICABLE_DIAGNOSTIC')
    def guard(phase):
        elapsed=observer.tick(phase,owned); legacy.log_guard(root)
        legacy.demand(elapsed<232 and reservation.previous+elapsed<1792,'budget cleanup reserve')
        return elapsed
    try:
        observer=Observer(output/'clock-timeline.jsonl')
        record['static']=static(args.wrk)
        record['static']['clock_symbol_clues']=[line.strip() for line in record['static']['clock_symbol_clues'] if any(k in line for k in ('gettimeofday','clock_gettime','time@'))]
        record['identity_before']=identity.snapshot(root,args.wrk)
        legacy.save(output/'run.json',record)
        began=guard('idle')
        while guard('idle')-began<120: time.sleep(.1)
        fixtures=output/'root'; fixtures.mkdir(); payload=legacy.fixture(fixtures,65536)
        item=dict(scenario='P4',round=0,**model.SCENARIOS['P4'])
        for label in ('C','D'):
            directory=output/('P4-'+label); directory.mkdir(); server=None; fatal=False
            row=dict(label=label,kind='diagnostic',status='invalid',payload=payload,schedule=dict(item,label=label))
            try:
                guard(label+'-start')
                manifest=record['identity_before']['manifests'][label]; legacy.invariant(manifest,fixtures,payload)
                server=legacy.OwnedProcess([manifest['binary'],*model.server_args(item),'--root',str(fixtures)],directory/'server')
                owned.append(server); row['server_identity']=server.identity
                began=guard(label+'-ready'); port=None
                while port is None:
                    legacy.demand(server.alive() and server.matches(),'server readiness failure')
                    legacy.demand(guard(label+'-ready')-began<5,'readiness timeout')
                    match=re.search(r'listening on port ([0-9]+)\.',server.stdout_path.read_text(errors='replace'))
                    if match:
                        port=int(match.group(1)); legacy.demand(legacy.listener_owned(server.process.pid,port),'listener ownership')
                    else: time.sleep(.1)
                row['pre_audit']=legacy.audit(port,payload)
                sample_flags=set(observer.flags)
                for phase,seconds in (('warmup',5),('measurement',20)):
                    guard(label+'-'+phase)
                    command=[str(args.wrk),*model.wrk_args(item),'--timeout','2s','--latency','-d',str(seconds)+'s','-s',str(legacy.HERE/'summary.lua'),f"http://127.0.0.1:{port}/{payload['name']}"]
                    worker=legacy.OwnedProcess(command,directory/phase); owned.append(worker)
                    first=clocks(); detail=dict(command=command,started_clocks=first,status='invalid')
                    try:
                        while True:
                            guard(label+'-'+phase)
                            legacy.demand(assess(first,first,clocks())[0]<=seconds+10,'diagnostic phase watchdog')
                            legacy.demand(server.alive() and server.matches(),'server exited')
                            if not worker.alive(): break
                            time.sleep(.1)
                        detail['ended_clocks']=clocks()
                        try:
                            detail['metrics']=legacy.parse_summary(worker.stdout_path.read_text(),worker.process.returncode,seconds,payload['size'])
                            detail['status']='observed'
                        except legacy.Invalid as error: detail['parse_error']=str(error)
                    finally:
                        detail['cleanup']=cleanup(worker,observer,root,label+'-'+phase+'-cleanup'); owned.remove(worker)
                        legacy.save(directory/(phase+'.diagnostic.json'),detail); row[phase]=detail
                    legacy.demand(not detail['cleanup']['forced'] and detail['cleanup']['reaped'],'cleanup anomaly')
                row['post_audit']=legacy.audit(port,payload); legacy.invariant(manifest,fixtures,payload)
                row['clock_flags']=sorted(observer.flags-sample_flags)
                row['status']='observed' if not row['clock_flags'] and all(row[p]['status']=='observed' for p in ('warmup','measurement')) else 'invalid'
            except BaseException as error:
                row['error']=type(error).__name__+': '+str(error)
                fatal=isinstance(error,(KeyboardInterrupt,SystemExit)) or any(k in str(error) for k in ('budget','log limit','cleanup','resource'))
            finally:
                if server:
                    row['cleanup']=cleanup(server,observer,root,label+'-server-cleanup'); owned.remove(server)
                    if row['cleanup']['forced'] or not row['cleanup']['reaped'] or row['cleanup']['returncode']!=0: fatal=True; row['status']='invalid'
                legacy.save(directory/'sample.json',row); record['samples'].append(row); legacy.save(output/'run.json',record)
            if fatal: raise legacy.Invalid('fatal budget/resource/cleanup anomaly')
        record['identity_after']=identity.snapshot(root,args.wrk)
        legacy.demand(record['identity_after']==record['identity_before'],'identity drift')
        record['status']='observed' if not observer.flags and all(r['status']=='observed' for r in record['samples']) else 'invalid'
    except BaseException as error: record['error']=type(error).__name__+': '+str(error)
    finally:
        for child in list(owned): cleanup(child,observer,root,'final-cleanup')
        if observer is not None:
            try: observer.tick('end')
            except BaseException as error:
                record['status']='invalid'
                record['final_clock_error']=str(error)
                observer.maximum=max(observer.maximum,reservation.allowance)
            elapsed=max(observer.maximum,time.monotonic()-reservation.began)
            flags=sorted(observer.flags)
            cpu=time.process_time()-observer.first['process_cpu']
            count=observer.sequence
            observer.stream.close()
        else:
            # Failed initialization has no usable timeline. Keep the full reserve.
            elapsed=reservation.allowance; flags=['clock initialization unknown']; cpu=None; count=0
        record.update(ended_utc=legacy.utc(),wall_seconds=elapsed,clock_flags=flags,
            observer_cpu_seconds=cpu,sample_count=count,role_log_bytes=legacy.log_bytes(root))
        terminal_status(record,flags)
        reservation.entry.update(state='finished',charged_seconds=elapsed,ended_utc=record['ended_utc'])
        budget.atomic(reservation.path,reservation.data); reservation.lock.close()
        legacy.save(output/'run.json',record)
    print(json.dumps(dict(status=record['status'],elapsed=elapsed,flags=record['clock_flags'])),flush=True)
    return int(record['status']!='observed')

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--role',choices=('builder','reviewer'),required=True)
    parser.add_argument('--wrk',type=pathlib.Path,required=True)
    parser.add_argument('--output',type=pathlib.Path,required=True)
    raise SystemExit(diagnose(parser.parse_args()))
