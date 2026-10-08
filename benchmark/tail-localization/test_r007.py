"""Deterministic R007 metadata regressions; no fork, mount, socket or host query."""
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from budget_v7 import Reservation, BudgetError
from root_marker_launcher_v2 import task_directory
from test_decode_binary import complete_events, fixture, event
from decode_v2 import decode


class R007Tests(unittest.TestCase):
    def test_backing_first_visible_755_with_restrictive_umask(self):
        with tempfile.TemporaryDirectory() as name:
            previous=os.umask(0o077)
            original=os.mkdir;visible=[]
            def observe(path,mode=0o777,*args,**kwargs):
                original(path,mode,*args,**kwargs)
                visible.append(Path(path).lstat().st_mode & 0o777)
            try:
                with patch('root_marker_launcher_v2.os.mkdir',side_effect=observe):
                    directory=task_directory(Path(name))
                self.assertEqual(visible,[0o755])
                self.assertEqual(directory.lstat().st_mode & 0o777,0o755)
                self.assertEqual(os.umask(0o077),0o077)
            finally:os.umask(previous)

    def test_early_idle_recording_window_before_future_warmup(self):
        with tempfile.TemporaryDirectory() as name:
            directory=Path(name);server,client=complete_events()
            server += [event(20,5,1),event(21,6,8192),event(22,7,0,1,11),event(23,16,1)]
            fixture(directory,server,client)
            sample=json.loads((directory/'sample.json').read_text())
            sample['recording_start_ns']=10;sample['warmup_start_ns']=50
            (directory/'sample.json').write_text(json.dumps(sample))
            self.assertEqual(decode(directory)['complete_requests'],1)

    def stage(self,name,unknown='run-smoke-001',bind=True):
        stage=Path(name);(stage/'leader').mkdir();(stage/'builder').mkdir()
        original=dict(run_id=unknown,kind='smoke',status='invalid',reserved_seconds=20,charged_seconds=20,
                      byte_classification_status='unknown',output_bytes=None,error='original EACCES')
        (stage/'builder/ledger.json').write_text(json.dumps(dict(schema=1,role='builder',runs=[original])))
        receipt=dict(authority='Approved S4 R007',scope=str(stage/'builder'),bytes_upper_bound=1024)
        path=stage/'leader/r007-current-builder-upper-bound.json';path.write_text(json.dumps(receipt))
        auth=dict(roles_serial=True,per_role_dynamic_seconds=100,per_role_output_bytes=2*1024*1024,
                  old_s3_ledgers=dict(builder={},reviewer={}),builder_http_limits=dict(smoke=2),reviewer_http_limits={})
        if bind:auth['r007_byte_reclassification']=dict(role='builder',run_id='run-smoke-001',sha256=hashlib.sha256(path.read_bytes()).hexdigest())
        (stage/'leader/authorization.json').write_text(json.dumps(auth))
        return stage,original

    def test_fixed_receipt_does_not_rewrite_failed_history(self):
        with tempfile.TemporaryDirectory() as name:
            stage,original=self.stage(name)
            with Reservation(stage,'builder','run-r007-test','selfcheck',1):pass
            saved=json.loads((stage/'builder/ledger.json').read_text())
            self.assertEqual(saved['runs'][0],original)

    def test_other_unknown_and_unbound_receipt_stop(self):
        for unknown,bind in [('run-smoke-001',False),('run-future-unknown',True)]:
            with tempfile.TemporaryDirectory() as name:
                stage,original=self.stage(name,unknown,bind)
                with self.assertRaises(BudgetError):
                    with Reservation(stage,'builder','run-r007-test','selfcheck',1):pass
                self.assertEqual(json.loads((stage/'builder/ledger.json').read_text())['runs'],[original])


if __name__=='__main__':unittest.main()
