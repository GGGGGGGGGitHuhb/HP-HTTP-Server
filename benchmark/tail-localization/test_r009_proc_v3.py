"""R009 actual helpers/control flow; all proc, clocks, processes and signals mocked."""
import errno
from contextlib import ExitStack,nullcontext
import io
import os
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch,MagicMock
import cleanup_v11 as cleanup
import localize_v12 as wrapper
import proc_identity_v11 as proc
import watchdog_v11 as watchdog
from inner_identity_v11 import CleanupIdentity
from budget_v10 import atomic_json


def record(pid=9002,start=20):return dict(pid=pid,starttime=start,ppid=9000,state='S',session=pid,pgrp=pid)

class ProcTests(unittest.TestCase):
    def test_absent_only(self):
        for error in (FileNotFoundError(errno.ENOENT,'gone'),ProcessLookupError(errno.ESRCH,'gone')):
            with patch.object(Path,'read_text',side_effect=error):self.assertIsNone(proc.process_identity(3))
        for code in (errno.EACCES,errno.EPERM,errno.EIO):
            with patch.object(Path,'read_text',side_effect=OSError(code,'fault')),self.assertRaises(OSError) as raised:proc.process_identity(3)
            self.assertEqual(raised.exception.target_pid,3)
        with patch.object(Path,'read_text',return_value='invalid'),self.assertRaises((ValueError,IndexError)):proc.process_identity(3)
    def test_scan_absent_and_reused(self):
        original=record()
        with patch.object(proc,'proc_candidates',return_value=[7]),patch.object(proc,'process_identity',side_effect=lambda pid:None if pid==7 else original):
            self.assertEqual(proc.owned_descendants(original,[]),[original])
        with patch.object(proc,'process_identity',return_value=record(start=21)):self.assertFalse(proc.same_process(original))
    def test_inner_continuous_unknown(self):
        state=CleanupIdentity();error=OSError(errno.EIO,'fault');error.target_pid=7
        with patch('inner_identity_v11.process_identity',side_effect=error):
            with self.assertRaises(OSError):state.alive(record(7))
            state.cleaning=True
            for _ in range(8):self.assertFalse(state.alive(record(7)))
            self.assertEqual(list(state.unknown),[7])
            with patch('inner_identity_v11.proc_candidates',return_value=[7]):self.assertEqual(state.owned(1,2),[])

class WrapperFinalizationTests(unittest.TestCase):
    def fixture(self,mode):
        with tempfile.TemporaryDirectory(prefix='r009-proc-fixture-') as name:
            repository=Path(name);stage=repository/'.cache/v0.5.1-s4';(stage/'leader').mkdir(parents=True);(stage/'builder').mkdir()
            auth=dict(roles_serial=True,per_role_dynamic_seconds=1200,per_role_output_bytes=2**31,builder_http_limits={},old_s3_ledgers=dict(builder='fixture'),r008=dict(builder_subbudget_seconds=360,builder_m3_reserved_seconds=570))
            (stage/'leader/authorization.json').write_text(json.dumps(auth))
            args=SimpleNamespace(role='builder',run_id='run-r008-proc-fixture',kind='selfcheck',seconds=20,static_inventory_sha256=None,static_inventory_name='static-inventory-v6.json',command=['never-started-real-workload'])
            supervisor=MagicMock(pid=9001,returncode=0);child=MagicMock(pid=9002,returncode=None)
            successful_work=mode in ('normal','pid-reused','watchdog-failed','pipe-write','evidence-write','command-nonzero')
            child.poll.return_value=0 if successful_work else None
            if successful_work:child.returncode=0
            if mode=='watchdog-failed':supervisor.returncode=1
            if mode=='command-nonzero':child.returncode=1;child.poll.return_value=1
            child.wait.side_effect=lambda **kw:setattr(child,'returncode',0)
            error=OSError(errno.EIO,'first work fault');error.target_pid=9003 if mode=='scan-target-priority' else 9002
            cleanup_error=PermissionError(errno.EACCES,'persistent cleanup fault');cleanup_error.target_pid=9002
            writes=[]
            def evidence(path,value):
                writes.append(path.name)
                if mode=='command-write' and path.name=='command.json':raise OSError(errno.EIO,'command evidence failure')
                if mode=='evidence-write' and path.name=='cleanup.json':raise OSError(errno.EIO,'cleanup evidence failure')
                atomic_json(path,value)
            def first_identity(pid):
                if pid==9002 and mode=='root-unknown':raise error
                return record(pid)
            real_close=os.close
            def close_fake(fd):
                if fd not in (10001,10002):return real_close(fd)
            ticks=iter(100+i*.01 for i in range(1000))
            with ExitStack() as stack:
                stack.enter_context(patch.object(wrapper.time,'monotonic',side_effect=lambda:next(ticks)))
                libc=stack.enter_context(patch.object(wrapper.ctypes,'CDLL'))
                stack.enter_context(patch.object(wrapper.signal,'signal'))
                stack.enter_context(patch.object(wrapper.signal,'getsignal',return_value=signal_token()))
                stack.enter_context(patch.object(wrapper.os,'pipe',return_value=(10001,10002)))
                stack.enter_context(patch.object(wrapper.os,'close',side_effect=close_fake))
                stack.enter_context(patch.object(wrapper,'process_identity',side_effect=first_identity))
                stack.enter_context(patch.object(wrapper,'owned_descendants',side_effect=error))
                stack.enter_context(patch.object(wrapper.subprocess,'Popen',side_effect=[supervisor,child]))
                stack.enter_context(patch.object(wrapper,'verify_protected',return_value=dict(match=True)))
                stack.enter_context(patch.object(wrapper,'atomic_json',side_effect=evidence))
                stack.enter_context(patch.object(cleanup,'owned_descendants',side_effect=cleanup_error if mode in ('continuous','root-unknown') else None,return_value=[]))
                stack.enter_context(patch.object(cleanup,'proc_candidates',return_value=[]))
                stack.enter_context(patch.object(cleanup,'process_identity',side_effect=cleanup_error if mode in ('continuous','root-unknown') else lambda pid:record(pid,start=21) if mode=='pid-reused' else None))
                kills=stack.enter_context(patch.object(cleanup.os,'kill'))
                stack.enter_context(patch.object(cleanup.os,'waitpid',side_effect=ChildProcessError))
                stack.enter_context(patch.object(cleanup.os,'close',side_effect=close_fake))
                notifications=stack.enter_context(patch.object(cleanup.os,'write',side_effect=OSError(errno.EIO,'notification failure') if mode=='pipe-write' else lambda fd,data:len(data)))
                stderr=stack.enter_context(patch.object(cleanup.sys,'stderr',new_callable=io.StringIO))
                libc.return_value.prctl.return_value=0
                with (nullcontext() if mode in ('normal','pid-reused') else self.assertRaises((OSError,wrapper.BudgetError))):wrapper.execute(args,repository)
                ledger=json.loads((stage/'builder/ledger.json').read_text())
                self.assertEqual(ledger['runs'][0]['status'],'valid' if mode in ('normal','pid-reused') else 'invalid')
                self.assertGreaterEqual(ledger['runs'][0]['charged_seconds'],0)
                self.assertLessEqual(ledger['runs'][0]['charged_seconds'],20)
                self.assertIn('protection-after.json',writes);self.assertIn('cleanup.json',writes)
                if mode in ('continuous','root-unknown'):
                    value=json.loads((stage/'builder/run-r008-proc-fixture/cleanup.json').read_text())
                    self.assertEqual(value['first_error']['type'],'OSError');self.assertEqual(value['first_error']['errno'],errno.EIO)
                    self.assertEqual(value['first_error']['pid'],9002)
                    self.assertTrue(value['unknown']);self.assertFalse(value['complete']);self.assertFalse(value['watchdog_notified'])
                    notifications.assert_not_called();kills.assert_not_called();supervisor.wait.assert_called_once();child.wait.assert_called_once()
                    self.assertLessEqual(supervisor.wait.call_args.kwargs['timeout'],7)
                    self.assertLessEqual(value['deadline'],ledger['runs'][0]['start_monotonic']+20)
                if mode=='scan-target-priority':
                    value=json.loads((stage/'builder/run-r008-proc-fixture/cleanup.json').read_text())
                    self.assertEqual(child.pid,9002)
                    self.assertEqual(value['first_error']['type'],'OSError')
                    self.assertEqual(value['first_error']['pid'],9003)
                if mode=='command-nonzero':
                    value=json.loads((stage/'builder/run-r008-proc-fixture/cleanup.json').read_text())
                    self.assertEqual(value['first_error']['type'],'BudgetError')
                    self.assertEqual(value['first_error']['pid'],9002)
                if mode=='pid-reused':kills.assert_not_called();notifications.assert_called_once_with(10002,b'D')
                if mode=='pipe-write':notifications.assert_called_once_with(10002,b'D')
                if mode=='evidence-write':self.assertIn('evidence_written',stderr.getvalue())
                return ledger
    def test_scan_target_overrides_command_pid(self):self.fixture('scan-target-priority')
    def test_command_failure_keeps_target_pid(self):self.fixture('command-nonzero')
    def test_reused_pid_never_signaled(self):self.fixture('pid-reused')
    def test_normal_disappearance_valid(self):self.fixture('normal')
    def test_watchdog_nonzero_invalid(self):self.fixture('watchdog-failed')
    def test_work_and_cleanup_continuous_failure(self):self.fixture('continuous')
    def test_root_identity_acquisition_failure(self):self.fixture('root-unknown')
    def test_prepare_evidence_failure_still_finalizes(self):self.fixture('command-write')
    def test_notification_failure_invalid(self):self.fixture('pipe-write')
    def test_cleanup_evidence_failure_independent_output(self):self.fixture('evidence-write')

def signal_token():return 0

class WatchdogTests(unittest.TestCase):
    def test_actual_identity_helper_is_shared(self):self.assertIs(watchdog.identity,proc.process_identity)
    def test_watchdog_failure_unknown_nonzero_no_unknown_signal(self):
        with tempfile.TemporaryDirectory() as name:
            clock=iter(i*.1 for i in range(100));error=OSError(errno.EIO,'watchdog read fault');error.target_pid=7
            def read(pid):
                if pid==7:raise error
                return record(pid)
            with patch.object(watchdog.time,'monotonic',side_effect=lambda:next(clock)),patch.object(watchdog.select,'select',return_value=([],[],[])),patch.object(watchdog,'proc_candidates',return_value=[7]),patch.object(watchdog,'identity',side_effect=read),patch.object(watchdog.os,'getpid',return_value=999),patch.object(watchdog.os,'kill') as kill:
                self.assertNotEqual(watchdog.supervise(9000,20,.5,2,3,Path(name)/'watchdog.json'),0)
                self.assertTrue(all(call.args[0]!=7 for call in kill.call_args_list))
                result=json.loads((Path(name)/'watchdog.json').read_text());self.assertTrue(result['unknown']);self.assertTrue(result['errors']);self.assertEqual(result['deadline'],2)
    def test_confirmed_D_normal(self):
        with patch.object(watchdog.time,'monotonic',return_value=0),patch.object(watchdog.select,'select',return_value=([3],[],[])),patch.object(watchdog.os,'read',return_value=b'D'):
            self.assertEqual(watchdog.supervise(1,2,5,7,3,'not-written'),0)


class AdditionalFailureTests(unittest.TestCase):
    def test_independent_output_failure_explicit(self):
        broken=MagicMock();broken.write.side_effect=OSError(errno.EIO,'stderr fault')
        with patch.object(cleanup.sys,'stderr',broken),patch.object(cleanup.os,'write',side_effect=OSError(errno.EIO,'fd2 fault')):
            self.assertFalse(cleanup.emit_error(dict(evidence_written=False)))
    def test_inner_resources_all_attempted_after_close_failure(self):
        from inner_identity_v11 import close_sample_resources,cleanup_evidence
        channel=MagicMock();mapping=MagicMock();streams=[MagicMock(),MagicMock()];errors=[]
        channel.close.side_effect=OSError(errno.EIO,'channel fault');streams[0].close.side_effect=OSError(errno.EIO,'stream fault')
        with patch('inner_identity_v11.os.close',side_effect=OSError(errno.EIO,'fd fault')),patch('signal.signal') as restore:
            close_sample_resources(3,channel,streams,mapping,{15:0},errors)
            streams[1].close.assert_called_once();mapping.close.assert_called_once();restore.assert_called_once();self.assertEqual(len(errors),3)
        with patch.object(cleanup.sys,'stderr',new_callable=io.StringIO) as output:
            self.assertFalse(cleanup_evidence(Path('not-written'),{},MagicMock(side_effect=OSError(errno.EIO,'file fault')),errors))
            self.assertIn('evidence_written',output.getvalue())
    def test_unsafe_and_unknown_D_rejected(self):
        for fault in (False,True):
            with self.subTest(fault=fault),tempfile.TemporaryDirectory() as name:
                ticks=iter(i*.1 for i in range(100));candidate=record(7);candidate['ppid']=9000
                error=OSError(errno.EIO,'candidate fault');error.target_pid=7
                def read(pid):
                    if pid==7 and fault:raise error
                    return candidate if pid==7 else record(pid)
                with patch.object(watchdog.time,'monotonic',side_effect=lambda:next(ticks)),patch.object(watchdog.select,'select',side_effect=[([],[],[]),([3],[],[])]),patch.object(watchdog.os,'read',return_value=b'D'),patch.object(watchdog,'identity',side_effect=read),patch.object(watchdog,'proc_candidates',return_value=[7]),patch.object(watchdog.os,'getpid',return_value=999),patch.object(watchdog.os,'kill'):
                    self.assertNotEqual(watchdog.supervise(9000,20,5,10,3,Path(name)/'watchdog.json'),0)
                    result=json.loads((Path(name)/'watchdog.json').read_text());self.assertEqual(result['reason'],'unsafe completion')


class FinalWatchdogEvidenceTests(unittest.TestCase):
    def test_final_owner_read_and_signal_errors_in_evidence(self):
        for read_failed in (True,False):
            with self.subTest(read_failed=read_failed),tempfile.TemporaryDirectory() as name:
                error=OSError(errno.EIO,'final fault');error.target_pid=9000
                with patch.object(watchdog.time,'monotonic',return_value=2),patch.object(watchdog,'identity',side_effect=error if read_failed else None,return_value=record(9000)),patch.object(watchdog.os,'kill',side_effect=error) as kills:
                    self.assertNotEqual(watchdog.supervise(9000,20,.5,1,3,Path(name)/'final.json'),0)
                    value=json.loads((Path(name)/'final.json').read_text())
                    if read_failed:self.assertTrue(value['unknown']);kills.assert_not_called()
                    else:self.assertTrue(value['errors']);self.assertEqual(value['errors'][0]['pid'],9000)

if __name__=='__main__':unittest.main()
