#!/usr/bin/env python3
"""Synthetic failure probes; no performance results are produced."""
import argparse
from contextlib import ExitStack
import importlib.util
import json
import os
import pathlib
import tempfile
import unittest
from unittest import mock
import diagnose as diag
import timeline
import intermediate

spec = importlib.util.spec_from_file_location('legacy_tests', diag.REPO/'tests/benchmark_runner_tests.py')
legacy_tests = importlib.util.module_from_spec(spec)
spec.loader.exec_module(legacy_tests)
BenchTests = legacy_tests.BenchTests

class DiagnosticTests(unittest.TestCase):
    def check_intermediate_identity(self, drift):
        initial_tool=dict(sha256='binary',libraries={'lib':'hash'},version='fixed',version_exit=1,ldd='lib (0x123)')
        final_tool=dict(initial_tool,ldd='lib (0x456)')
        if drift=='binary':final_tool['sha256']='changed'
        if drift=='library':final_tool['libraries']={'lib':'changed'}
        initial_scripts={'entry':'hash'}
        final_scripts={'entry':'changed'} if drift=='shared-script' else initial_scripts
        with tempfile.TemporaryDirectory(dir=os.environ['TMPDIR']) as directory:
            role=pathlib.Path(directory)
            output=role/'run-intermediate'
            argv=['intermediate.py','--role','builder','--wrk','synthetic','--output',str(output)]
            with ExitStack() as patches:
                patches.enter_context(mock.patch.object(diag.sys,'argv',argv))
                patches.enter_context(mock.patch.object(diag,'role_root',return_value=role))
                patches.enter_context(mock.patch.object(diag.legacy,'environment',return_value={'kind':'synthetic'}))
                patches.enter_context(mock.patch.object(diag.legacy,'validate_manifest',return_value={'label':'S1'}))
                patches.enter_context(mock.patch.object(diag.legacy,'validate_tool',side_effect=[initial_tool,final_tool]))
                patches.enter_context(mock.patch.object(diag,'identities',side_effect=[initial_scripts,final_scripts]))
                patches.enter_context(mock.patch.object(diag.builds,'sha',side_effect=['self-hash','changed' if drift=='self-script' else 'self-hash']))
                patches.enter_context(mock.patch.object(diag.legacy,'run_sample',return_value={'status':'valid','measurement':{'qps':100}}))
                code=intermediate.main()
            result=json.loads((output/'run.json').read_text())
            self.assertEqual(len(result['samples']),3)
            self.assertFalse((output/'root').exists())
            if drift:
                self.assertEqual(code,1)
                self.assertEqual(result['status'],'invalid')
                self.assertIn('identity drift',result['error'])
                self.assertNotIn('qps_median',result)
            else:
                self.assertEqual(code,0)
                self.assertEqual(result['status'],'valid')
                self.assertEqual(result['qps_median'],100)

    def test_intermediate_aslr_change_valid(self):
        self.check_intermediate_identity(None)

    def test_intermediate_binary_drift_invalid(self):
        self.check_intermediate_identity('binary')

    def test_intermediate_library_drift_invalid(self):
        self.check_intermediate_identity('library')

    def test_intermediate_shared_script_drift_invalid(self):
        self.check_intermediate_identity('shared-script')

    def test_intermediate_self_script_drift_invalid(self):
        self.check_intermediate_identity('self-script')

    def test_ldd_aslr_is_not_tool_identity(self):
        tool=dict(sha256='binary',libraries={'lib':'hash'},version='fixed',version_exit=1,ldd='lib (0x123)')
        changed=dict(tool,ldd='lib (0x456)')
        self.assertEqual(diag.stable_tool_identity(tool),diag.stable_tool_identity(changed))
        changed['libraries']={'lib':'drift'}
        self.assertNotEqual(diag.stable_tool_identity(tool),diag.stable_tool_identity(changed))

    def test_timeline_deadline(self):
        with mock.patch.object(timeline.socket,'create_connection') as connect:
            with self.assertRaisesRegex(diag.legacy.Invalid,'10 second group budget'):
                timeline.receive_series(1,{},False,0)

    def test_timeline_bad_status(self):
        with mock.patch.object(timeline.socket,'create_connection') as connect:
            connect.return_value.__enter__.return_value.recv.return_value=b'HTTP/1.1 500 Error\r\n\r\n'
            with self.assertRaisesRegex(diag.legacy.Invalid,'bad status'):
                timeline.receive_series(1,{'name':'fixture','size':0},False,float('inf'))

    def test_path_boundary(self):
        with self.assertRaisesRegex(diag.legacy.Invalid, 'outside role root'):
            diag.checked_output('builder','/tmp/outside-diagnosis')

    def test_partial_c_rejected(self):
        with self.assertRaisesRegex(diag.legacy.Invalid, 'incomplete suite'):
            diag.summary([],('C',))

    def test_invalid_identity_saved(self):
        with tempfile.TemporaryDirectory(dir=os.environ['TMPDIR']) as directory:
            root = pathlib.Path(directory)
            args = argparse.Namespace(role='builder', output=str(root/'run-failure'),suite='C',wrk='missing')
            with mock.patch.object(diag,'role_root',return_value=root), mock.patch.object(diag.legacy,'validate_manifest',side_effect=diag.legacy.Invalid('identity mismatch')):
                self.assertEqual(diag.run_suite(args),1)
            result=json.loads((root/'run-failure/run.json').read_text())
            self.assertEqual(result['status'],'invalid')
            self.assertNotIn('summary',result)

    def test_cumulative_dynamic_budget(self):
        with tempfile.TemporaryDirectory(dir=os.environ['TMPDIR']) as directory:
            root=pathlib.Path(directory)
            (root/'run-old').mkdir()
            (root/'run-old/run.json').write_text(json.dumps({'wall_seconds':1801}))
            args=argparse.Namespace(role='builder',output=str(root/'run-budget'),suite='C',wrk='missing')
            with mock.patch.object(diag,'role_root',return_value=root):
                self.assertEqual(diag.run_suite(args),1)
            result=json.loads((root/'run-budget/run.json').read_text())
            self.assertIn('30 minute',result['error'])
            self.assertEqual(result['status'],'invalid')

if __name__=='__main__':
    unittest.main()
