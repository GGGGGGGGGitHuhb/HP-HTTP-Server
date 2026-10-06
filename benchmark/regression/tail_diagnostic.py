#!/usr/bin/env python3
"""R003 single bounded diagnostic; never a formal performance verdict."""
import argparse
import json
import os
import pathlib
import shutil
import signal
import sys
import time
import budget
import executor
import identity
import model
import supervision
from identity import legacy


def schedule():
    return [dict(round=i, scenario=p, label='D', **(model.SCENARIOS[p] | {'threads':t}))
            for i,(p,t) in enumerate((('P1',1),('P3',2),('P3',4),('P3',4),('P3',2),('P1',1)),1)]


def read(path):
    try:
        return {'value':pathlib.Path(path).read_text(), 'error':None}
    except (OSError, ValueError) as error:
        return {'value':None, 'error':type(error).__name__+': '+str(error)}


def stat_identity(text):
    fields=text[text.rfind(')')+2:].split()
    return {'starttime':int(fields[19]), 'utime_ticks':int(fields[11]), 'stime_ticks':int(fields[12]), 'state':fields[0]}


def thread(pid, tid):
    base=pathlib.Path(f'/proc/{pid}/task/{tid}')
    row={'tid':tid, 'stat':read(base/'stat'), 'schedstat':read(base/'schedstat'),
         'status':read(base/'status'), 'wchan':read(base/'wchan')}
    try:
        row['identity']=stat_identity(row['stat']['value']) if row['stat']['value'] else None
    except (ValueError, IndexError) as error:
        row['identity']=None;row['parse_error']=str(error)
    if row['status']['value'] is not None:
        row['status']['value']='\n'.join(line for line in row['status']['value'].splitlines() if line.startswith(('Name:', 'State:', 'voluntary_ctxt_switches:', 'nonvoluntary_ctxt_switches:', 'Cpus_allowed_list:')))
    return row


def adjacent_delta(previous, current):
    """Never join different PID/TID incarnations or silently replace missing values."""
    if any(previous.get(k)!=current.get(k) for k in ('pid','tid','starttime')):
        return None
    values={}
    for key in ('runtime_ns','wait_ns','timeslices','utime_ticks','stime_ticks','voluntary','nonvoluntary'):
        a,b=previous.get(key),current.get(key)
        values[key]=b-a if isinstance(a,int) and isinstance(b,int) and b>=a else None
    return values


class Observer:
    def __init__(self, path):
        self.path=path;self.stream=path.open('x');self.processes=[];self.next=0.;self.last=None
        self.enabled=True;self.count=0;self.max_gap=0.;self.cpu=0.;self.failure=None
        self.cgroup=read('/proc/self/cgroup')
        self.cgroup_path=None
        for line in (self.cgroup['value'] or '').splitlines():
            if line.startswith('0::'):
                candidate=(pathlib.Path('/sys/fs/cgroup')/line[3:].lstrip('/')).resolve()
                if candidate.is_relative_to('/sys/fs/cgroup'):self.cgroup_path=candidate/'cpu.stat'

    def tick(self):
        # Observability may never interrupt OwnedProcess.close()/wait(). Fatal sampler
        # failure latches invalid and stops after the current bounded sample cleanup.
        if not self.enabled or self.failure or time.monotonic()<self.next:return
        began=time.process_time()
        try:
            now=time.monotonic();self.next=now+.1
            if self.last is not None:self.max_gap=max(self.max_gap,now-self.last)
            self.last=now
            row={'sequence':self.count,'monotonic':now,'processes':[],
                 'system_stat':read('/proc/stat'), 'loadavg':read('/proc/loadavg'),
                 'cgroup_cpu_stat':read(self.cgroup_path) if self.cgroup_path else {'value':None,'error':'cgroup v2 path unavailable'}}
            for owned,prefix in self.processes:
                pid=owned.process.pid
                if owned.process.poll() is not None:continue
                proc={'pid':pid,'starttime':owned.identity['starttime'],'prefix':str(prefix),'threads':[],
                      'io':read(f'/proc/{pid}/io'),'stat':read(f'/proc/{pid}/stat')}
                try:
                    proc['identity_matches']=stat_identity(proc['stat']['value'])['starttime']==owned.identity['starttime']
                    if not proc['identity_matches']:
                        self.failure='process identity drift'
                        raise ValueError(self.failure)
                    proc['affinity']=sorted(os.sched_getaffinity(pid))
                    proc['threads']=[thread(pid,int(p.name)) for p in pathlib.Path(f'/proc/{pid}/task').iterdir() if p.name.isdigit()]
                except (OSError,ValueError,TypeError,IndexError) as error:
                    proc['observation_error']=type(error).__name__+': '+str(error)
                proc['log_bytes']={suffix:(pathlib.Path(str(prefix)+suffix).stat().st_size if pathlib.Path(str(prefix)+suffix).exists() else None) for suffix in ('.stdout','.stderr')}
                row['processes'].append(proc)
            self.stream.write(json.dumps(row)+'\n');self.stream.flush();self.count+=1
            # Timeline has its own strict ceiling; legacy.guard covers stdout/stderr.
            legacy.demand(self.path.stat().st_size<=64*1024*1024,'observer timeline limit')
        except BaseException as error:
            if isinstance(error,(KeyboardInterrupt,SystemExit)):raise
            self.failure=type(error).__name__+': '+str(error)
        finally:self.cpu+=time.process_time()-began

    def close(self):
        try:self.stream.close()
        except OSError as error:self.failure=self.failure or str(error)
        return {'samples':self.count,'max_gap_seconds':self.max_gap,'sampling_cpu_seconds':self.cpu,
                'error':self.failure,'clock_ticks_per_second':os.sysconf('SC_CLK_TCK'),'cgroup':self.cgroup,'sched_schedstats':read('/proc/sys/kernel/sched_schedstats')}


def observed_process(observer):
    class ObservedProcess(legacy.OwnedProcess):
        def __init__(self,command,prefix):
            super().__init__(command,prefix)
            observer.processes.append((self,prefix))
        def alive(self):
            observer.tick()
            return super().alive()
    return ObservedProcess


def cleanup_all(observer):
    observer.enabled=False
    rows=[]
    for owned,prefix in observer.processes:
        try:
            result=owned.close()
            rows.append({'prefix':str(prefix), **result})
        except BaseException as error:
            rows.append({'prefix':str(prefix),'error':type(error).__name__+': '+str(error), 'reaped':False})
    return rows


def interrupted(signum, frame):
    signal.signal(signal.SIGINT,signal.SIG_IGN);signal.signal(signal.SIGTERM,signal.SIG_IGN)
    raise KeyboardInterrupt('diagnostic interrupted '+str(signum))


def run(args):
    root=identity.role_root('builder')
    legacy.demand(not any(budget.load(p).get('kind')=='diagnostic-tail' for p in root.glob('run-*/run.json')),'one R003 diagnostic only')
    reservation=budget.Reservation(root,args.output,'diagnostic-tail',240)
    record={'run_id':reservation.id,'kind':'diagnostic-tail','status':'invalid','performance_acceptance':'NOT_APPLICABLE_DIAGNOSTIC',
            'started_utc':legacy.utc(),'schedule':schedule(),'samples':[],'prior_dynamic_seconds':reservation.previous}
    fixture=reservation.output/'root';observer=None;old=executor.OwnedProcess
    handlers={sig:signal.getsignal(sig) for sig in (signal.SIGTERM,signal.SIGINT)}
    try:
        for sig in handlers:signal.signal(sig,interrupted)
        record['preflight']=supervision.resources(root)
        record['identity_before']=identity.snapshot(root,args.wrk)
        legacy.demand(record['identity_before']['manifests']['D']['commit']==identity.COMMITS['D'],'fixed D required')
        legacy.save(reservation.output/'run.json',record)
        fixture.mkdir();payload=legacy.fixture(fixture,1024)
        observer=Observer(reservation.output/'threads.jsonl');executor.OwnedProcess=observed_process(observer)
        for i,item in enumerate(schedule(),1):
            reservation.guard()
            row=executor.run_sample(record['identity_before']['manifests']['D'],args.wrk,fixture,payload,
                                    reservation.output/f'{i:02d}-{item["scenario"]}-t{item["threads"]}',root,
                                    reservation.began+240,item,5,20)
            record['samples'].append(row);legacy.save(reservation.output/'run.json',record)
            legacy.demand(observer.failure is None,'observer failed: '+str(observer.failure))
        record['identity_after']=identity.snapshot(root,args.wrk)
        legacy.demand(record['identity_before']==record['identity_after'],'identity drift')
        reservation.guard();record['status']='observed'
    except BaseException as error:record['error']=type(error).__name__+': '+str(error)
    finally:
        executor.OwnedProcess=old
        for sig in handlers:signal.signal(sig,signal.SIG_IGN)
        if observer:
            record['final_cleanup']=cleanup_all(observer)
            if any(not c.get('reaped') or c.get('forced') or c.get('error') for c in record['final_cleanup']):record['status']='invalid'
        if observer:
            record['observer']=observer.close()
            if observer.failure:record['status']='invalid'
        record.update(ended_utc=legacy.utc(),wall_seconds=time.monotonic()-reservation.began)
        legacy.save(reservation.output/'run.json',record)
        reservation.finish()
        if fixture.exists():shutil.rmtree(fixture)
        for sig,handler in handlers.items():signal.signal(sig,handler)
    print(json.dumps({'status':record['status'],'samples':len(record['samples']),'error':record.get('error')}))
    return int(record['status']!='observed')


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--wrk',required=True);p.add_argument('--output',required=True)
    sys.exit(run(p.parse_args()))
