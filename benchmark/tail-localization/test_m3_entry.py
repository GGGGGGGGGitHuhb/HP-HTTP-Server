"""Necessary real-helper counterexamples; no sockets, signals or /proc probes."""
import errno
import io
import json
from pathlib import Path
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import m3_contract as contract
import m3_metrics as metrics
from m3_staircase_owned import matches_owner,adapt_executor
from m3_compare import compare


def stdout(raw='S4 raw latency us: count=100 p50=10 p99=20 max=30',errors=None):
    row=dict(schema=1,duration_us=20_000_000,requests=100,bytes=102400,errors=errors or dict(connect=0,read=0,write=0,status=0,timeout=0),latency_us=dict(mean=10,p50=10,p95=15,p99=20,max=30))
    return 'BENCH_SUMMARY '+json.dumps(row)+'\n'+raw+'\n'


class M3EntryTests(unittest.TestCase):
    def test_global_raw_count_and_bounds(self):
        self.assertTrue(metrics.summary(stdout(),0)['raw_latency_available'])
        for raw in ('','S4 raw latency us: count=99 p50=10 p99=20 max=30','S4 raw latency us: count=100 p50=25 p99=20 max=30','S4 raw latency us: count=100 p50=10 p99=20 max=2000001'):
            with self.subTest(raw=raw),self.assertRaises(ValueError):metrics.summary(stdout(raw),0)
        with self.assertRaises(ValueError):metrics.summary(stdout()+stdout().splitlines()[1]+'\n',0)

    def test_summary_error_nonfinite_and_duration(self):
        with self.assertRaises(ValueError):metrics.summary(stdout(errors=dict(connect=0,read=1,write=0,status=0,timeout=0)),0)
        with self.assertRaises(ValueError):metrics.summary(stdout().replace('20000000','Infinity'),0)
        with self.assertRaises(ValueError):metrics.summary(stdout().replace('20000000','1000000'),0)
        with self.assertRaises(ValueError):metrics.summary(stdout(),1)

    def test_absence_errno_and_pid_reuse(self):
        owner=dict(pid=123,starttime=5)
        for number in (errno.ENOENT,errno.ESRCH):
            def missing(pid):raise OSError(number,'gone')
            self.assertFalse(matches_owner(missing,owner))
        self.assertFalse(matches_owner(lambda pid:dict(starttime=6),owner))
        for number in (errno.EACCES,errno.EPERM,errno.EIO):
            def fault(pid):raise OSError(number,'unknown')
            with self.assertRaises(OSError):matches_owner(fault,owner)
        with self.assertRaises(ValueError):matches_owner(lambda pid:(_ for _ in ()).throw(ValueError('parse')),owner)

    def test_proc_metric_absence_and_unknown(self):
        owner=dict(pid=123,starttime=5)
        for number in (errno.ENOENT,errno.ESRCH,errno.EACCES):
            fake=SimpleNamespace(read_text=lambda number=number:(_ for _ in ()).throw(OSError(number,'fault')))
            with patch.object(metrics,'Path',return_value=fake):
                if number==errno.EACCES:
                    with self.assertRaises(OSError):metrics.process_metrics(owner)
                else:self.assertIsNone(metrics.process_metrics(owner))

    def test_exact_contract_order_role_configuration(self):
        with tempfile.TemporaryDirectory() as tmp,patch.object(contract,'ROOT',Path(tmp)):
            role=Path(tmp)/'.cache/v0.5.1-s4/builder';role.mkdir(parents=True);out=role/contract.STAIRS[0];out.mkdir()
            row=dict(run_id=out.name,kind='staircase',status='running',reserved_seconds=45,output=str(out),start_monotonic=0)
            def save(rows):(role/'ledger.json').write_text(json.dumps(dict(runs=rows)))
            save([row]);self.assertEqual(contract.admit('builder',out,'staircase')[1],0)
            for rows in ([row,row],[dict(row,run_id=contract.STAIRS[1])],[dict(row,reserved_seconds=30)],[row,dict(row,run_id='other')]):
                save(rows)
                with self.assertRaises(ValueError):contract.admit('builder',out,'staircase')
            save([row])
            with self.assertRaises(ValueError):contract.admit('reviewer',out,'staircase')
            abba=role/contract.ABBA[0];abba.mkdir();save([dict(row,run_id=abba.name,kind='observed_abba',output=str(abba))])
            with self.assertRaises(ValueError):contract.admit('builder',abba,'observed_abba')

    def test_cpu_actual_envelope_and_sampled_rss(self):
        first=dict(pid=1,starttime=2,time_ns=100,cpu_seconds=1,rss_kib=9)
        client=dict(pid=3,starttime=4,time_ns=100,cpu_seconds=2,rss_kib=8)
        last=dict(first,time_ns=1_000_000_100,cpu_seconds=1.5)
        row=metrics.finish_resources([dict(server=first,client=client)],last,2.25,100)
        self.assertEqual(row['server_cpu_percent_one_core'],50)
        self.assertEqual(row['client_cpu_percent_one_core'],25)
        self.assertEqual(row['client_rss_sampled_max_kib'],8)
        with self.assertRaises(ValueError):metrics.finish_resources([],last,2.25,100)

    def test_close_identity_error_still_waits_and_closes(self):
        seen=[]
        class Process:
            returncode=None
            def poll(self):return self.returncode
            def wait(self,timeout):seen.append('wait');self.returncode=0;return 0
            def terminate(self):seen.append('unsafe-term')
            def kill(self):seen.append('unsafe-kill')
        class Base:
            def __init__(self):self.process=Process();self.identity=dict(pid=123,starttime=5);self.stdout=SimpleNamespace(close=lambda:seen.append('stdout'));self.stderr=SimpleNamespace(close=lambda:seen.append('stderr'));self.stdout_path=Path('/synthetic.stdout');self.forced=False
            def alive(self):return self.process.poll() is None
        def reader(pid):raise OSError(errno.EACCES,'unknown')
        executor=SimpleNamespace(OwnedProcess=Base,process_info=reader,save=lambda p,v:seen.append(('evidence',v.copy())))
        adapt_executor(executor,time.monotonic()+5)
        with patch('m3_staircase_owned.emit_error',return_value=True),self.assertRaises(RuntimeError):executor.OwnedProcess().close()
        self.assertNotIn('unsafe-term',seen);self.assertNotIn('unsafe-kill',seen)
        self.assertTrue(all(x in seen for x in ('wait','stdout','stderr')))
        evidence=next(x[1] for x in seen if isinstance(x,tuple));self.assertTrue(evidence['reaped']);self.assertTrue(evidence['unknown']);self.assertEqual(evidence['first_error']['errno'],errno.EACCES)

    def test_close_evidence_failure_preserves_first(self):
        class Base:
            def __init__(self):self.process=SimpleNamespace(returncode=0,wait=lambda timeout:0);self.identity=dict(pid=123,starttime=5);self.stdout=io.StringIO();self.stderr=io.StringIO();self.stdout_path=Path('/synthetic.stdout');self.forced=False
            def alive(self):return False
        def fail(p,v):raise OSError(errno.EIO,'evidence')
        executor=SimpleNamespace(OwnedProcess=Base,process_info=lambda pid:dict(starttime=5),save=fail);adapt_executor(executor,time.monotonic()+5)
        owner=executor.OwnedProcess()
        with patch('m3_staircase_owned.emit_error') as emit,self.assertRaises(RuntimeError):owner.close()
        self.assertTrue(owner.stdout.closed and owner.stderr.closed)
        self.assertEqual(emit.call_args.args[0]['staircase_cleanup']['first_error']['errno'],errno.EIO)

    def test_global_raw_warning_keeps_selected_chain_separate(self):
        perf=metrics.summary(stdout(),0)
        a=dict(run_id='a',parameters=dict(detailed=False),performance=perf,measurement_slow_requests=[])
        b=dict(run_id='b',parameters=dict(detailed=True),performance=dict(perf,raw_latency_ms=dict(count=100,p50=.01,p99=.04,max=.05)),measurement_slow_requests=[])
        row=compare([],[a,b]);self.assertTrue(row['warnings']);self.assertFalse(row['core_evidence_candidate']);self.assertEqual(row['comparison']['global_raw_p99_percent'],100)

    def test_a_offline_actual_headers_control_and_clock(self):
        import ctypes
        from decode_v3 import HEADER
        from wire_types_v3 import Control
        from m3_offline import verify
        with tempfile.TemporaryDirectory() as tmp:
            directory=Path(tmp);control=Control();control.magic=b'S4CTRL03';control.version=3;control.bytes=ctypes.sizeof(Control);control.phase=5;control.connections=128;control.selectedCount=4;control.serverRegistered=control.clientRegistered=control.serverAttempts=control.clientAttempts=128;control.warmupNs=5_000_000_000;control.measurementNs=20_000_000_000
            for rows in (control.serverRegistrations,control.clientRegistrations):
                for row in rows:row.ready=1
            mappings=[]
            for endpoint,count in (('server',4),('client',2)):
                for worker in range(count):
                    pid=100 if endpoint=='server' else 200;tid=pid+worker
                    mappings.append(dict(endpoint=endpoint,role='worker',worker=worker,pid=pid,namespace_tid=tid,kernel_tid=tid,starttime=5))
                    (directory/f'{endpoint}-worker-{worker}.events.bin').write_bytes(HEADER.pack(b'S4TAIL02',64,32,1,0,16*1024*1024//32,pid,tid,5,0,0))
                    table=control.serverThreads if endpoint=='server' else control.clientThreads;table[worker].stoppedNs=30_000_000_000
                mappings.append(dict(endpoint=endpoint,role='main',worker=count,pid=pid,namespace_tid=pid,kernel_tid=pid,starttime=5,identity=dict(boot_id='boot',time_namespace='ns')))
                (directory/f'{endpoint}-clock.json').write_text(json.dumps(dict(pid=pid,boot_id='boot',time_namespace='ns',clock='CLOCK_MONOTONIC',unit='ns')))
            (directory/'startup-control.bin').write_bytes(bytes(control));(directory/'client.stdout').write_text(stdout());(directory/'marker-launcher.json').write_text(json.dumps(dict(status='valid',marker_lost_events=0)))
            sample=dict(schema=3,status='valid',identity_scope='kernel-mapped',kernel_mapping_verified=True,parameters=dict(requests_per_connection=0,connections=128,warmup=5,duration=20,detailed=False),recording_start_ns=1,recording_end_ns=30_000_000_000,measurement_start_ns=5_000_000_000,measurement_end_ns=25_000_000_000,frozen_connections=[{}]*4,kernel_mappings=mappings,cleanup=dict(forced=False,errors=[],remaining=[]),manifest=dict(role='builder'),performance=dict(metrics.summary(stdout(),0),resources={}))
            def save():(directory/'sample.json').write_text(json.dumps(sample))
            save();result=verify(directory);self.assertIsNone(result['complete_requests']);self.assertEqual(len(result['file_catalog']),6);self.assertTrue(result['performance']['raw_latency_available'])
            sample['recording_end_ns']=24_000_000_000;save()
            with self.assertRaises(ValueError):verify(directory)
            sample['recording_end_ns']=30_000_000_000;save();control.phase=4;(directory/'startup-control.bin').write_bytes(bytes(control))
            with self.assertRaises(ValueError):verify(directory)
            control.phase=5;(directory/'startup-control.bin').write_bytes(bytes(control));(directory/'client-clock.json').write_text(json.dumps(dict(pid=200,boot_id='wrong',time_namespace='ns',clock='CLOCK_MONOTONIC',unit='ns')))
            with self.assertRaises(ValueError):verify(directory)
