#!/usr/bin/env python3
"""Approved R004: immutable D, file/null/null/file stderr, one diagnostic only."""
import argparse
import json
import os
import pathlib
import shutil
import signal
import stat
import subprocess
import sys
import time
import budget
import executor
import identity
import model
import supervision
import tail_diagnostic as tail
from identity import legacy


def schedule():
    return [dict(round=i,scenario='P3',label='D',stderr_sink=sink,**model.SCENARIOS['P3'])
            for i,sink in enumerate(('file','null','null','file'),1)]


def sink_process(observer, sink):
    legacy.demand(sink in ('file','null'),'invalid sink')
    class SinkProcess(legacy.OwnedProcess):
        def __init__(self,command,prefix):
            is_null=sink=='null' and pathlib.Path(prefix).name=='server'
            if not is_null:
                super().__init__(command,prefix)
            else:
                device=os.stat('/dev/null')
                legacy.demand(stat.S_ISCHR(device.st_mode) and device.st_rdev==os.makedev(1,3),'unexpected null device')
                self.command=command
                self.stdout_path=pathlib.Path(str(prefix)+'.stdout');self.stderr_path=pathlib.Path('/dev/null')
                self.stdout=self.stdout_path.open('wb');self.stderr=None
                try:
                    self.stderr=self.stderr_path.open('wb')
                    self.process=subprocess.Popen(command,stdout=self.stdout,stderr=self.stderr)
                    self.identity=legacy.process_info(self.process.pid)
                    self.forced=False;self.stop_result=None
                    target=os.readlink(f'/proc/{self.process.pid}/fd/2')
                    child_stat=os.stat(f'/proc/{self.process.pid}/fd/2')
                    legacy.demand(stat.S_ISCHR(child_stat.st_mode) and child_stat.st_rdev==device.st_rdev,'child null device mismatch')
                    legacy.demand(target=='/dev/null','child stderr did not target null')
                    legacy.save(str(prefix)+'.process.json',{'command':command,**self.identity,'stderr_sink':'null','stderr_fd_target':target,'stderr_device':child_stat.st_rdev,'stderr_inode':child_stat.st_ino,'stderr_bytes':None,'stderr_bytes_note':'production volume uncounted; output discarded'})
                except BaseException:
                    if hasattr(self,'process'):
                        self.process.kill();self.process.wait(timeout=3)
                    self.stdout.close()
                    if self.stderr:self.stderr.close()
                    raise
            observer.processes.append((self,prefix))
        def alive(self):
            observer.tick()
            return super().alive()
    return SinkProcess


def run(args):
    root=identity.role_root('builder')
    legacy.demand(not any(budget.load(p).get('kind')=='diagnostic-sink' for p in root.glob('run-*/run.json')),'one R004 diagnostic only')
    reservation=budget.Reservation(root,args.output,'diagnostic-sink',140)
    record={'run_id':reservation.id,'kind':'diagnostic-sink','status':'invalid','performance_acceptance':'NOT_APPLICABLE_DIAGNOSTIC',
            'started_utc':legacy.utc(),'schedule':schedule(),'samples':[],'prior_dynamic_seconds':reservation.previous}
    fixture=reservation.output/'root';observer=None;old=executor.OwnedProcess
    handlers={sig:signal.getsignal(sig) for sig in (signal.SIGTERM,signal.SIGINT)}
    try:
        for sig in handlers:signal.signal(sig,tail.interrupted)
        record['preflight']=supervision.resources(root)
        record['prior_log_bytes']=legacy.log_bytes(root)
        legacy.demand(legacy.LOG_LIMIT-record['prior_log_bytes']>=300*1024*1024,'less than 300MiB planned log headroom')
        record['identity_before']=identity.snapshot(root,args.wrk)
        legacy.demand(record['identity_before']['manifests']['D']['commit']==identity.COMMITS['D'],'fixed D required')
        legacy.save(reservation.output/'run.json',record)
        fixture.mkdir();payload=legacy.fixture(fixture,1024)
        observer=tail.Observer(reservation.output/'threads.jsonl')
        for i,item in enumerate(schedule(),1):
            reservation.guard();executor.OwnedProcess=sink_process(observer,item['stderr_sink'])
            row=executor.run_sample(record['identity_before']['manifests']['D'],args.wrk,fixture,payload,
                                    reservation.output/f'{i:02d}-P3-{item["stderr_sink"]}',root,
                                    reservation.began+140,item,5,20)
            row['stderr_sink']=item['stderr_sink'];row['stderr_produced_bytes']=None
            row['stderr_note']='null volume uncounted' if item['stderr_sink']=='null' else 'file bytes indexed in log evidence'
            legacy.save(reservation.output/f'{i:02d}-P3-{item["stderr_sink"]}'/'sample.json',row)
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
            record['final_cleanup']=tail.cleanup_all(observer)
            if any(not c.get('reaped') or c.get('forced') or c.get('error') for c in record['final_cleanup']):record['status']='invalid'
            record['observer']=observer.close()
            if observer.failure:record['status']='invalid'
        record.update(ended_utc=legacy.utc(),wall_seconds=time.monotonic()-reservation.began)
        legacy.save(reservation.output/'run.json',record);reservation.finish()
        if fixture.exists():shutil.rmtree(fixture)
        for sig,handler in handlers.items():signal.signal(sig,handler)
    print(json.dumps({'status':record['status'],'samples':len(record['samples']),'error':record.get('error')}))
    return int(record['status']!='observed')


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--wrk',required=True);p.add_argument('--output',required=True)
    sys.exit(run(p.parse_args()))
