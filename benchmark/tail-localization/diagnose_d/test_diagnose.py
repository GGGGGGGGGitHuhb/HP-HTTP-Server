"""One paid offline suite: no socket, D process, or workload invocation."""
import copy
import io
import json
from pathlib import Path
import subprocess
import sys
import time
import unittest
from unittest import mock
sys.dont_write_bytecode=True
import importlib.util
spec=importlib.util.spec_from_file_location('r033_collect_fixture',Path(__file__).with_name('collect.py'))
collect=importlib.util.module_from_spec(spec)
spec.loader.exec_module(collect)
analyze=collect.analysis


def fixture():
    t0=10000000000;t1=t0+20000000000
    raw=[{1000:254,60000:2},{1000:254,60000:1},{60000:1},{1000:1},{1000:1}]
    intervals=[20000000//(sum(raw[i].values())//128) for i in (0,1)]
    bins=raw+[analyze.reference.reconstruct_correction(raw[i],intervals[i]) for i in (0,1)]
    metadata=[]
    for i,value in enumerate(bins):
        source=raw[i-5] if i>=5 else value
        low=min(source,default=analyze.reference.UINT64_MAX)
        metadata.append({'file':analyze.reference.NAMES[i],'count':sum(value.values()),'scan_min_us':low,
            'max_us':max(value,default=0),'p50_us':analyze.reference.original_percentile(value,low,50),
            'p99_us':analyze.reference.original_percentile(value,low,99)})
    client={'schema':'baseline-v1-client','status':'valid','clock':'CLOCK_MONOTONIC','warm_start_ns':t0-5000000000,
            'T0_ns':t0,'T1_ns':t1,'duration_ns':20000000000,'N':256,'request_bytes':100,'interval_us':intervals,
            'histograms':metadata,'threads':[]}
    for owner in (0,1):
        records=[{'life':1,'sequence':2,'start_ns':t0-10000000 if owner==0 else t0+10000000,
                  'complete_ns':t0+50000000 if owner==0 else t0+70000000,'class':1 if owner==0 else 2,
                  'status':200,'body_bytes':1024}]
        client['threads'].append({'owner':owner,'stopped_ns':t1,'completed':[1,1,127,1] if owner==0 else [0,0,128,0],
            'overflow':False,'slow_capacity':16384,'slow_records':records,
            'pending_at_T1':0,'pending_warm':0,'censored_partial':0,'censored_unsent':0,
            'censored_sent_deadline_after_T1':0,'censored_by_start_phase':[[0,0,0],[0,0,0]],
            'errors':{key:0 for key in ('connect','read','write','status','timeout','headers','body','protocol')},
            'connections':[{'life':i+1,'ready_ns':t0-5000000001,'closed':True,'active':False,
                'full_sent':True,'pending_at_T1':False,'start_ns':t0,'sequence':3,'sent_bytes':100,
                'wait_lower_bound_ns':0,'end_reason':2} for i in range(64)]})
    return client,bins


def snapshots():
    rows=[]
    for timestamp in (9000000000,10000000000,30000000000):
        threads=[]
        for index,role in enumerate(('server','client','observer')):
            threads.append({'role':role,'pid':index+1,'process_starttime':10,'tid':index+1,'thread_starttime':10,
                'run_ticks':timestamp//1000000,'sched_run_ns':timestamp,'sched_wait_ns':timestamp,
                'voluntary':timestamp//1000000,'nonvoluntary':0,'read_bytes':timestamp//1000000,
                'write_bytes':0,'wchan':'futex'})
        rows.append({'begin_ns':timestamp,'end_ns':timestamp+1,'threads':threads,'errors':[]})
    network=[{'begin_ns':row['begin_ns'],'end_ns':row['end_ns'],'errors':[]} for row in rows]
    return rows,network


class Contract(unittest.TestCase):
    def test_cross_window_full_raw_and_bins(self):
        client,bins=fixture()
        result=analyze.validate_client(client,bins)
        self.assertEqual(result['N'],256)
        self.assertEqual([r['class'] for r in result['slow_records']],[1,2])
        self.assertEqual([r['bin'] for r in result['completion_bins_5ms']],[10,14])
        self.assertFalse(result['owner_tid_mapping_available'])
        self.assertFalse(result['cause_confirmed'])

    def test_count_truncation_overflow_errors(self):
        for mutate in (lambda c:c.update(N=255),lambda c:c['threads'][0].update(pending_at_T1=1),
                       lambda c:c['threads'][0].update(overflow=True),lambda c:c['threads'][0]['errors'].update(body=1)):
            client,bins=fixture();mutate(client)
            with self.assertRaises(ValueError):analyze.validate_client(client,bins)

    def test_corrected_corruption(self):
        client,bins=fixture();bins[5][1000]+=1
        with self.assertRaisesRegex(ValueError,'corrected'):analyze.validate_client(client,bins)

    def test_negative_base_has_no_observe_permission(self):
        client,bins=fixture()
        for thread in client['threads']:
            thread['slow_records']=[]
        bins[0]={1000:256};bins[1]={1000:255};bins[2]={1000:1}
        for i in range(2):bins[i+5]=dict(bins[i])
        for i,value in enumerate(bins):
            low=min(bins[i-5] if i>=5 else value,default=analyze.reference.UINT64_MAX)
            client['histograms'][i].update(count=sum(value.values()),scan_min_us=low,max_us=max(value,default=0),
                p50_us=analyze.reference.original_percentile(value,low,50),p99_us=analyze.reference.original_percentile(value,low,99))
        self.assertFalse(analyze.validate_client(client,bins)['observe_permitted_by_evidence'])

    def test_thread_and_network_gaps_unknown(self):
        rows,network=snapshots()
        record={'start_ns':10000000000,'complete_ns':10060000000}
        result=analyze.correlate([record],rows,network,10000000000,30000000000)
        self.assertEqual(result['direction_status'],'unknown')
        self.assertTrue(result['thread_gaps']);self.assertTrue(result['network_gaps'])

    def test_read_disappearance_identity_clock_fail_closed(self):
        for mutate in (lambda r:r[1].update(errors=['EACCES']),lambda r:r[1].update(begin_ns=1),
                       lambda r:r[1]['threads'].pop(),lambda r:r[1]['threads'][0].update(thread_starttime=20),
                       lambda r:r[1]['threads'][0].update(sched_run_ns=-1)):
            rows,network=snapshots();mutate(rows)
            with self.assertRaises(ValueError):analyze.correlate([],rows,network,10000000000,30000000000)

    def test_real_normal_subtree_reaped(self):
        result=SCOPE.command(['/bin/sh','-c','sleep 0.01 & wait'])
        self.assertEqual(result,b'')
        self.assertIsNotNone(SCOPE.children[-1]['process'].returncode)

    def test_real_timeout_stops_and_reaps(self):
        local=collect.adapter.Scope(OUTPUT,5,6)
        local.work_deadline=min(local.work_deadline,time.monotonic()+.08)
        local.thread.start()
        try:
            with self.assertRaises(BaseException):local.command(['/bin/sleep','5'])
        finally:
            result=local.finish()
        self.assertFalse(result['remaining'])
        self.assertTrue(all(row.get('reaped',False) for row in result['children']))

    def test_publish_fault_preserves_cleanup_and_first(self):
        errors=[]
        local=collect.adapter.Scope(OUTPUT,5,6)
        local.command(['/bin/true'])
        with mock.patch.object(collect,'publish',side_effect=OSError('injected publish')), \
             mock.patch.object(sys,'stderr',new=BrokenStream()):
            cleanup=collect.finish_contract(OUTPUT,local,{},errors)
        self.assertTrue(cleanup['complete'])
        self.assertTrue(all(row.get('reaped',False) for row in cleanup['children']))
        self.assertEqual(errors[0]['message'],'injected publish')

    def test_capacity_and_real_interface_contract(self):
        with self.assertRaises(ValueError):collect.check_capacity(32*1024*1024+1,0,32*1024*1024)
        legacy=collect.adapter.load_legacy(SCOPE)
        import inspect
        self.assertEqual(inspect.signature(legacy.audit).parameters['count'].default,5)
        with mock.patch.object(legacy,'audit',return_value=[]) as audit:
            collect.audit_five(legacy,1234,{'name':'payload-1024.bin'})
            audit.assert_called_once_with(1234,{'name':'payload-1024.bin'},count=5)
        import re
        self.assertEqual(re.findall(r'listening on port (\d+)\.','listening on port 1234.'),['1234'])
        commands=collect.fixed_commands('builder',OUTPUT,1234)
        self.assertEqual(commands['client'][1:7],['-t2','-c128','-d','20s','--timeout','2s'])
        self.assertNotIn(';;',commands['client_environment']['LUA_PATH'])


class BrokenStream:
    def write(self,text):raise OSError('injected stderr')
    def flush(self):raise OSError('injected stderr flush')


def run_contract(output,scope):
    global OUTPUT,SCOPE
    OUTPUT,SCOPE=output,scope
    suite=unittest.defaultTestLoader.loadTestsFromTestCase(Contract)
    result=unittest.TextTestRunner(verbosity=1).run(suite)
    return {'tests':result.testsRun,'failures':len(result.failures),'errors':len(result.errors),
            'no_socket':True,'D_started':False,'fixture_only':True}


class Entry(unittest.TestCase):
    def test_main_dispatch_and_common_deadline(self):
        stage=OUTPUT/'cli-stage';(stage/'builder/control').mkdir(parents=True)
        class Scope:
            def __init__(self,*args):pass
            def poll(self):pass
            def finish(self):return {'complete':True,'remaining':[],'children':[]}
        for mode in ('base','observe'):
            work=time.monotonic_ns()+2000000000;cleanup=work+4000000000
            output=stage/'builder/control'/('r034-diagnose-'+mode+'-driver-001')
            argv=['collect.py','--role','builder','--mode',mode,'--output',str(output),
                  '--shared-work-deadline-ns',str(work),'--shared-cleanup-deadline-ns',str(cleanup)]
            with mock.patch.object(collect,'STAGE',stage),mock.patch.object(sys,'argv',argv),mock.patch.object(collect.adapter,'Scope',Scope),mock.patch.object(collect,'collect_live') as live,mock.patch.object(collect.adapter,'measure'),mock.patch.object(collect.adapter,'SHARED_WORK_DEADLINE'),mock.patch.object(collect.adapter,'SHARED_CLEANUP_DEADLINE'):
                collect.main()
                self.assertEqual(live.call_args.args[:3],('builder',mode,output))
                self.assertEqual(collect.adapter.SHARED_WORK_DEADLINE,work/1e9)
        args=collect.parse_arguments(argv[1:]);args.output=str(stage/'builder/control/wrong')
        with mock.patch.object(collect,'parse_arguments',return_value=args),mock.patch.object(collect,'STAGE',stage):
            with self.assertRaisesRegex(ValueError,'exact mode output'):collect.main()

    def test_real_mode_parser(self):
        for role,mode in (('builder','entry'),('reviewer','entry'),('builder','base'),('builder','observe')):
            args=collect.parse_arguments(['--role',role,'--mode',mode,'--output','unused',
                    '--shared-work-deadline-ns','1','--shared-cleanup-deadline-ns','2'])
            self.assertEqual((args.role,args.mode),(role,mode))
        with self.assertRaises(SystemExit):collect.parse_arguments(['--mode','formal'])

    def test_observer_open_failure(self):
        import threading
        stop=threading.Event();errors=[]
        with mock.patch.object(Path,'open',side_effect=OSError('observer open')):
            collect.sample_observer([],0,OUTPUT,stop,errors,time.monotonic_ns()+1000000000)
        self.assertTrue(stop.is_set());self.assertEqual(errors[0]['type'],'OSError')

    def test_observer_write_and_close_failure(self):
        import threading
        class Stream:
            def write(self,value):raise OSError('observer write')
            def close(self):raise OSError('observer close')
        stop=threading.Event();errors=[]
        with mock.patch.object(Path,'open',return_value=Stream()),mock.patch.object(collect,'thread_snapshot',return_value={}):
            collect.sample_observer([],0,OUTPUT,stop,errors,time.monotonic_ns()+1000000000)
        self.assertEqual(len(errors),3);self.assertTrue(stop.is_set())

    def test_thread_early_exit_and_coverage_rejection(self):
        import threading
        stop=threading.Event();errors=[]
        with mock.patch.object(Path,'open',return_value=io.StringIO()),mock.patch.object(collect,'thread_snapshot',side_effect=RuntimeError('early exit')):
            collect.sample_observer([],0,OUTPUT,stop,errors,time.monotonic_ns()+1000000000)
        self.assertEqual(errors[0]['type'],'RuntimeError')
        with self.assertRaises(ValueError):analyze.correlate([],[],[],10,20)

    def test_fixed_capacity_and_negative_gate(self):
        collect.check_capacity(1,1,536870912)
        with self.assertRaises(ValueError):collect.check_capacity(1,536870913,536870912)
        client,bins=fixture();result=analyze.validate_client(client,bins)
        self.assertTrue(result['observe_permitted_by_evidence'])
        client,bins=fixture();bins[0]={1000:256};bins[1]={1000:128};bins[2]={1000:128};bins[5]={1000:256};bins[6]={1000:128}
        for thread in client['threads']:
            thread['slow_records']=[]
            thread['completed']=[1,64,64,1] if thread['owner']==0 else [0,64,64,0]
        for index in (0,1,2,5,6):
            row=client['histograms'][index];row.update(count=sum(bins[index].values()),scan_min_us=1000,max_us=1000,p50_us=1000,p99_us=1000)
        result=analyze.validate_client(client,bins)
        self.assertFalse(result['base_reproduced'])

    def test_actual_sample_catalog_and_machine_offline(self):
        import hashlib
        import struct
        root=OUTPUT/'offline-repo';stage=root/'.cache/v0.5.1-revalidation'
        base=stage/'builder/control/r034-diagnose-base-driver-001'
        observed=stage/'builder/control/r034-diagnose-observe-driver-001'
        for directory,mode in ((base,'base'),(observed,'observe')):
            directory.mkdir(parents=True)
            client,bins=fixture()
            (directory/'client.json').write_text(json.dumps(client))
            (directory/'actual-command.json').write_text('{}')
            for name,histogram in zip(analyze.reference.NAMES,bins):
                (directory/name).write_bytes(b''.join(struct.pack('<QQ',key,value) for key,value in sorted(histogram.items())))
            if mode=='observe':
                threads,network=snapshots()
                (directory/'threads.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in threads))
                (directory/'network.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in network))
            catalog=[{'path':path.name,'bytes':path.stat().st_size,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()} for path in sorted(directory.iterdir())]
            audit={'status':200,'bytes':1024,'sha256':collect.PAYLOAD_SHA}
            sample={'schema':'r033-d-sample-v1','status':'valid','mode':mode,'server_commit':'69424e6ab057bba2950c018e34c5695a4dc74f22',
                    'server_sha256':collect.SERVER_SHA,'client_sha256':collect.CLIENT_SHA,'cleanup':{'complete':True},'first_error':None,'errors':[],
                    'result':{'pre_audit':[audit]*5,'post_audit':[audit]*5},'catalog':catalog}
            (directory/'sample.json').write_text(json.dumps(sample))
        result=analyze.analyze_sample(base)
        self.assertTrue(result['base_reproduced'])
        sample=json.loads((observed/'sample.json').read_text());sample['catalog']=[row for row in sample['catalog'] if row['path']!='threads.jsonl']
        (observed/'sample.json').write_text(json.dumps(sample))
        with self.assertRaises(ValueError):analyze.analyze_sample(observed)
        client,bins=fixture()
        bins[0]={1000:256};bins[1]={1000:128};bins[2]={1000:128};bins[5]={1000:256};bins[6]={1000:128}
        for thread in client['threads']:
            thread['slow_records']=[];thread['completed']=[1,64,64,1] if thread['owner']==0 else [0,64,64,0]
        for index in (0,1,2,5,6):client['histograms'][index].update(count=sum(bins[index].values()),scan_min_us=1000,max_us=1000,p50_us=1000,p99_us=1000)
        (base/'client.json').write_text(json.dumps(client))
        for name,histogram in zip(analyze.reference.NAMES,bins):
            (base/name).write_bytes(b''.join(struct.pack('<QQ',key,value) for key,value in sorted(histogram.items())))
        sample=json.loads((base/'sample.json').read_text())
        sample['catalog']=[{'path':path.name,'bytes':path.stat().st_size,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()} for path in sorted(base.iterdir()) if path.name!='sample.json']
        (base/'sample.json').write_text(json.dumps(sample))
        output=stage/'builder/control/r034-diagnose-offline-driver-001'
        work=time.monotonic_ns()+2000000000
        argv=['analyze.py','--role','builder','--output',str(output),'--sample',str(base),
              '--shared-work-deadline-ns',str(work),'--shared-cleanup-deadline-ns',str(work+4000000000)]
        with mock.patch.object(analyze,'ROOT',root),mock.patch.object(sys,'argv',argv):
            analyze.main()
        self.assertEqual(json.loads((output/'offline-result.json').read_text())['status'],'valid')
        sample['status']='invalid';(base/'sample.json').write_text(json.dumps(sample))
        rejected=stage/'reviewer/control/r034-diagnose-offline-driver-001';rejected.parent.mkdir()
        argv[argv.index('builder')]='reviewer';argv[argv.index(str(output))]=str(rejected)
        with mock.patch.object(analyze,'ROOT',root),mock.patch.object(sys,'argv',argv):
            with self.assertRaises(ValueError):analyze.main()
        self.assertEqual(json.loads((rejected/'offline-result.json').read_text())['status'],'invalid')


def run_entry(output,scope):
    global OUTPUT,SCOPE
    OUTPUT,SCOPE=output,scope
    result=unittest.TextTestRunner(verbosity=1).run(unittest.defaultTestLoader.loadTestsFromTestCase(Entry))
    return {'tests':result.testsRun,'failures':len(result.failures),'errors':len(result.errors),
            'no_socket':True,'D_started':False,'fixture_only':True}
