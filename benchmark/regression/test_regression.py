#!/usr/bin/env python3
"""Pure unit/fault probes; existing eight CTest cover real socket primitives."""
import argparse
import copy
import json
import os
import pathlib
import tempfile
import time
import signal
import subprocess
import sys
import threading
import supervision
import collect
import shutil
import unittest
from unittest import mock
from contextlib import ExitStack
import budget
import executor
import identity
import model
import regression
from identity import legacy


def sample(item):
    small = item['scenario'] in ('P1','P2','P3')
    d = item['label'] == 'D'
    metric = {'qps': 1000 if d and small else 100, 'latency_ms': {'p99': 25 if d and small else 100}, 'command': ['wrk', *model.wrk_args(item)], 'resources': {'server_cpu_percent_one_core': 20, 'wrk_cpu_percent_one_core': 10, 'rss_sampled_max_kib': 100}}
    row = {'status': 'valid', 'label': item['label'], 'schedule': item, 'server_command': ['server', *model.server_args(item)], 'warmup': copy.deepcopy(metric), 'measurement': metric,
        'payload': {'size': item['size'], 'sha256':'digest'}, 'pre_audit':[{'status':200,'bytes':item['size'],'sha256':'digest'}]*5, 'post_audit':[{'status':200,'bytes':item['size'],'sha256':'digest'}]*5, 'cleanup':{'reaped':True,'forced':False,'returncode':0}}
    sync(row)
    return row

def sync(row):
    for phase,seconds in (('warmup',5),('measurement',20)):
        m=row[phase]
        m.update(schema=1,duration_us=seconds*1000000,requests=round(m['qps']*seconds),bytes=round(m['qps']*seconds)*row['payload']['size'],errors=dict.fromkeys(('connect','read','write','status','timeout'),0),latency_us=dict.fromkeys(('mean','p50','p95','p99','max'),m['latency_ms']['p99']*1000))


class RegressionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=os.environ['TMPDIR'])
        self.root = pathlib.Path(self.temp.name)
    def tearDown(self):
        self.temp.cleanup()
    def rows(self):
        return [sample(item) for item in model.schedule()]
    def test_low_resources_before_spawn(self):
        for value in ('disk','memory','nofile'):
            with mock.patch.object(supervision.subprocess,'Popen') as spawn:
                if value=='nofile':
                    with mock.patch.object(legacy,'resources_ok',return_value={'nofile':512}):
                        with self.assertRaises(legacy.Invalid):supervision.run(['unused'],self.root,self.root/'low')
                else:
                    with mock.patch.object(legacy,'resources_ok',side_effect=legacy.Invalid(value)):
                        with self.assertRaises(legacy.Invalid):supervision.run(['unused'],self.root,self.root/'low')
                spawn.assert_not_called()
    def descendant_probe(self,fault):
        marker=self.root/'descendant.json'
        code=("import subprocess,sys,time,pathlib,json,os; "
              "p=subprocess.Popen([sys.executable,'-c','import time;time.sleep(60)'], start_new_session=True); "
              "fields=pathlib.Path('/proc/'+str(p.pid)+'/stat').read_text().rsplit(')',1)[1].split(); "
              f"pathlib.Path({str(marker)!r}).write_text(json.dumps(dict(pid=p.pid,starttime=int(fields[19])))); "
              + ("sys.stdout.write('x'*65536);sys.stdout.flush();" if fault=='log' else '') + "time.sleep(60)")
        outsider=subprocess.Popen([sys.executable,'-c','import time;time.sleep(60)'],start_new_session=True)
        timer=None
        def interrupt():
            deadline=time.monotonic()+3
            while not marker.exists() and time.monotonic()<deadline:time.sleep(.01)
            os.kill(os.getpid(),signal.SIGINT)
        try:
            if fault=='interrupt':
                timer=threading.Thread(target=interrupt);timer.start()
            original=legacy.log_guard
            with ExitStack() as stack:
                if fault=='log':stack.enter_context(mock.patch.object(legacy,'log_guard',side_effect=lambda root:original(root,32768)))
                with self.assertRaises(KeyboardInterrupt if fault=='interrupt' else legacy.Invalid):
                    supervision.run([sys.executable,'-c',code],self.root,self.root/'probe',timeout=.4 if fault=='timeout' else 5)
            if timer:timer.join(timeout=4)
            self.assertTrue(marker.exists())
            descendant=json.loads(marker.read_text())
            self.assertFalse(pathlib.Path('/proc/'+str(descendant['pid'])).exists(),'owned descendant leaked')
            cleanup=json.loads((self.root/'probe.cleanup.json').read_text())
            self.assertTrue(cleanup['reaped'])
            self.assertFalse(cleanup['forced'])
            self.assertIsNone(outsider.poll(),'unrelated process signalled')
        finally:
            if timer:timer.join(timeout=4)
            outsider.terminate();outsider.wait(timeout=3)
    def test_real_descendant_timeout(self):self.descendant_probe('timeout')
    def test_real_descendant_interrupt(self):self.descendant_probe('interrupt')
    def test_real_descendant_log_limit(self):self.descendant_probe('log')
    def test_public_archive_is_not_dynamic_ledger(self):
        role=self.root/'role';role.mkdir()
        run=role/'run-original';run.mkdir()
        record={'run_id':'real','kind':'formal','status':'invalid','wall_seconds':12,'ended_utc':'now'}
        legacy.save(run/'run.json',record)
        (run/'server.stderr').write_bytes(b'x'*65536)
        (run/'measurement.stdout').write_text('original wrk evidence')
        ledger={'schema':1,'entries':{'real':{'state':'finished','charged_seconds':12,'reserved_seconds':900}}}
        legacy.save(role/'ledger.json',ledger)
        archive=self.root/'benchmark/results/V0.5.1/S3/export'
        with mock.patch.object(identity,'REPO',self.root),mock.patch.object(identity,'role_root',return_value=role),mock.patch.object(sys,'argv',['collect','--role','builder','--output',str(archive)]):
            collect.main()
        published=archive/'run-original/archive-run.json'
        self.assertEqual(published.read_bytes(),(run/'run.json').read_bytes())
        self.assertFalse(list(archive.rglob('run.json')))
        self.assertFalse((archive/'run-original/server.stderr').exists())
        self.assertEqual((archive/'run-original/measurement.stdout').read_text(),'original wrk evidence')
        self.assertEqual(budget.load(archive/'summary.json')['log_hashes']['run-original/server.stderr']['bytes'],65536)
        shutil.copytree(archive,role/'D/source/benchmark/results/V0.5.1/S3/old')
        self.assertEqual(budget.charged(role,ledger),12)
        nested=role/'tmp/renamed';nested.parent.mkdir()
        run.rename(nested)
        self.assertEqual(budget.charged(role,ledger),12)
        unknown=role/'tmp/other';unknown.mkdir()
        legacy.save(unknown/'run.json',{'status':'invalid','ended_utc':'now','wall_seconds':33})
        self.assertEqual(budget.charged(role,ledger),45)
    def test_schedule_exact(self):
        rows = model.schedule()
        self.assertEqual(len(rows), 30)
        self.assertEqual([(r['scenario'],r['label']) for r in rows[:10]], [(p,l) for p in ('P1','P2','P3','P4','P5') for l in ('C','D')])
        self.assertEqual([(r['scenario'],r['label']) for r in rows[10:20]], [(p,l) for p in ('P5','P4','P3','P2','P1') for l in ('D','C')])
        self.assertEqual([(r['scenario'],r['label']) for r in rows[20:]], [(p,l) for p in ('P3','P4','P5','P1','P2') for l in ('C','D')])
    def test_real_parameter_propagation(self):
        for name, config in model.SCENARIOS.items():
            item = dict(config, scenario=name, label='D', round=1)
            with mock.patch.object(executor,'process_info',return_value={}), mock.patch.object(executor,'OwnedProcess',side_effect=RuntimeError('captured')) as process:
                with self.assertRaisesRegex(RuntimeError,'captured'):
                    executor.wrk_run('wrk',mock.Mock(process=mock.Mock(pid=1)),1,{'name':'payload'},1,self.root/'wrk',self.root,time.monotonic()+10,item)
                command = process.call_args.args[0]
                self.assertEqual(command[1:3], [f"-t{config['threads']}", f"-c{config['connections']}"])
            with mock.patch.object(legacy,'invariant'), mock.patch.object(executor,'OwnedProcess',side_effect=RuntimeError('captured')) as process:
                with self.assertRaisesRegex(RuntimeError,'captured'):
                    executor.run_sample({'binary':'server','label':'D','commit':'x'},'wrk',self.root,{},self.root/name,self.root,time.monotonic()+10,item)
                command = process.call_args.args[0]
                self.assertEqual(command[command.index('--threads')+1],str(config['workers']))
    def test_gate_boundaries(self):
        rows = self.rows()
        for row in rows:
            if row['label']=='D' and row['schedule']['scenario'] in ('P4','P5'):
                row['measurement']['qps']=90
                row['measurement']['latency_ms']['p99']=125
        for row in rows:sync(row)
        self.assertEqual(model.aggregate(rows)['failures'],[])
        for scenario in model.SCENARIOS:
            for metric in ('qps','p99'):
                changed=copy.deepcopy(rows)
                for row in changed:
                    if row['label']=='D' and row['schedule']['scenario']==scenario:
                        if metric=='qps':row['measurement']['qps']-=.05
                        else:row['measurement']['latency_ms']['p99']+=.05
                for row in changed:sync(row)
                self.assertIn(scenario+': numerical gate',model.aggregate(changed)['failures'])
    def test_span_boundary_and_noise(self):
        rows=self.rows()
        selected=[r for r in rows if r['schedule']['scenario']=='P1' and r['label']=='C']
        for row,value in zip(selected,(90,100,110)):row['measurement']['qps']=value
        for row in rows:sync(row)
        self.assertEqual(model.aggregate(rows)['failures'],[])
        selected[2]['measurement']['qps']=110.05
        sync(selected[2])
        self.assertIn('P1C: noisy',model.aggregate(rows)['failures'])
    def test_missing_duplicate_and_wrong_parameters(self):
        for fault in ('missing','duplicate','worker','wrk'):
            rows=self.rows()
            if fault=='missing':rows.pop()
            elif fault=='duplicate':rows[1]=rows[0]
            elif fault=='worker':rows[0]['server_command'][2]='2'
            else:rows[0]['measurement']['command'][1]='-t2'
            with self.assertRaises(legacy.Invalid):model.aggregate(rows)
    def test_untrusted_valid_status(self):
        for fault in ('label','nan','negative','zero','p99zero','errors','audit','cleanup','bytes','derived'):
            rows=self.rows();row=rows[0]
            if fault=='label':row['label']='D'
            elif fault=='nan':row['measurement']['qps']=float('nan')
            elif fault=='negative':row['measurement']['qps']=-1
            elif fault=='zero':row['measurement']['qps']=0
            elif fault=='p99zero':row['measurement']['latency_ms']['p99']=0
            elif fault=='errors':row['measurement']['errors']['read']=1
            elif fault=='audit':row['post_audit'][0]={'status':200,'bytes':1024,'sha256':'wrong'}
            elif fault=='cleanup':row['cleanup']['forced']=True
            elif fault=='bytes':row['measurement']['bytes']=1
            else:row['measurement']['qps']=999
            with self.subTest(fault=fault),self.assertRaises(legacy.Invalid):model.aggregate(rows)
    def test_stable_aslr_real_identity(self):
        value={'sha256':'a','libraries':{'lib':'b'},'version':'x','version_exit':1,'ldd':'addr1'}
        self.assertEqual(identity.stable_tool(value),identity.stable_tool(dict(value,ldd='addr2')))
        for key,new in [('sha256','c'),('libraries',{'lib':'c'})]:
            self.assertNotEqual(identity.stable_tool(value),identity.stable_tool(dict(value,**{key:new})))
    def test_output_paths(self):
        self.assertEqual(budget.new_output(self.root,self.root/'run-ok'),self.root/'run-ok')
        (self.root/'link').symlink_to(self.root,target_is_directory=True)
        for path in (self.root/'tmp/run-x',self.root/'link/run-x',self.root/'wrong',self.root.parent/'run-escape'):
            with self.assertRaises(legacy.Invalid):budget.new_output(self.root,path)
        (self.root/'run-link').symlink_to(self.root/'missing')
        with self.assertRaises(legacy.Invalid):budget.new_output(self.root,self.root/'run-link')
    def test_pre_reservation_failure_charge_and_lock(self):
        first=budget.Reservation(self.root,self.root/'run-a','formal')
        self.assertEqual(budget.charged(self.root,budget.load(first.path)),900)
        with self.assertRaises(BlockingIOError):budget.Reservation(self.root,self.root/'run-b','formal')
        first.began-=12
        elapsed=first.finish()
        self.assertGreaterEqual(elapsed,12)
        self.assertGreaterEqual(budget.charged(self.root,budget.load(first.path)),12)
    def test_incomplete_rename_nested_and_exhausted(self):
        run=budget.Reservation(self.root,self.root/'run-a','formal')
        run.lock.close() # crash: reservation remains, even if record is moved
        nested=self.root/'tmp/renamed';nested.parent.mkdir()
        run.output.rename(nested)
        self.assertEqual(budget.charged(self.root,budget.load(run.path)),900)
        other=budget.Reservation(self.root,self.root/'run-b','formal')
        other.lock.close()
        with self.assertRaisesRegex(legacy.Invalid,'exhausted'):budget.Reservation(self.root,self.root/'run-c','smoke',60)
    def test_recursive_unknown_and_corrupt(self):
        nested=self.root/'tmp/renamed';nested.mkdir(parents=True)
        path=nested/'run.json'
        legacy.save(path,{'ended_utc':'now','wall_seconds':123,'status':'invalid'})
        self.assertEqual(budget.charged(self.root,{'schema':1,'entries':{}}),123)
        path.write_text('{')
        with self.assertRaises(legacy.Invalid):budget.Reservation(self.root,self.root/'run-a','formal')
    def test_corrupt_ledger_fails_before_output(self):
        (self.root/'ledger.json').write_text('{')
        with self.assertRaises(legacy.Invalid):budget.Reservation(self.root,self.root/'run-a','formal')
        self.assertFalse((self.root/'run-a').exists())
    def test_log_and_time_budget(self):
        run=budget.Reservation(self.root,self.root/'run-a','smoke',60)
        try:
            with mock.patch.object(legacy,'log_bytes',return_value=legacy.LOG_LIMIT+1):
                with self.assertRaisesRegex(legacy.Invalid,'log limit'):run.guard()
            run.began-=61
            with self.assertRaisesRegex(legacy.Invalid,'watchdog'):run.guard()
        finally:run.finish()
    def probe(self,fault=None,smoke=False,ad_hoc=False):
        commits=dict(identity.COMMITS)
        if ad_hoc:commits['D']='future'
        legacy.save(self.root/'refs.json',{'commits':commits,'trees':identity.TREES,'requested_ref':'future' if ad_hoc else None})
        before={'manifests':{l:{'label':l} for l in ('C','D')},'identity':'same'}
        after=copy.deepcopy(before)
        if fault=='drift':after['identity']='changed'
        def execute(manifest,tool,root,payload,directory,output,deadline,item,*durations):
            if fault=='audit':raise legacy.Invalid('tail audit')
            row=sample(item)
            if fault=='gate' and item['label']=='D':
                row['measurement']['qps']=1
                sync(row)
            return row
        args=argparse.Namespace(role='builder',command='smoke' if smoke else 'run',output=str(self.root/'run-probe'),wrk='fake')
        with ExitStack() as stack:
            stack.enter_context(mock.patch.object(identity,'role_root',return_value=self.root))
            stack.enter_context(mock.patch.object(identity,'snapshot',side_effect=[before,after]))
            stack.enter_context(mock.patch.object(legacy,'environment',return_value={}))
            stack.enter_context(mock.patch.object(executor,'run_sample',side_effect=execute))
            code=regression.run_suite(args)
        record=budget.load(self.root/'run-probe/run.json')
        self.assertEqual(code,1 if fault else 0)
        if fault:self.assertNotIn('summary',record)
        if smoke or ad_hoc:self.assertNotEqual(record.get('performance_acceptance'),'PASS')
        self.assertFalse((self.root/'run-probe/root').exists())
    def test_suite_success(self):self.probe()
    def test_suite_identity_failure(self):self.probe('drift')
    def test_suite_audit_failure(self):self.probe('audit')
    def test_suite_numerical_failure(self):self.probe('gate')
    def test_smoke_not_formal(self):self.probe(smoke=True)
    def test_adhoc_not_formal(self):self.probe(ad_hoc=True)

if __name__=='__main__':unittest.main()
