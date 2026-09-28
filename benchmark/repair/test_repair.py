#!/usr/bin/env python3
"""Synthetic repair-runner failures; never counted as measured performance."""
import argparse
from contextlib import ExitStack
import importlib.util
import json
import os
import pathlib
import tempfile
import unittest
from unittest import mock
import repair
import mechanism

spec=importlib.util.spec_from_file_location('old_tests',repair.REPO/'tests/benchmark_runner_tests.py')
old=importlib.util.module_from_spec(spec);spec.loader.exec_module(old)
BenchTests=old.BenchTests


def sample(manifest,tool,root,payload,directory,output,deadline):
    value=1000 if manifest['label']=='D' and payload['size']==1024 else 100
    p99=1 if manifest['label']=='D' and payload['size']==1024 else 100
    return {'status':'valid','label':manifest['label'],'payload':payload,'measurement':{'qps':value,'latency_ms':{'p99':p99}}}


class RepairTests(unittest.TestCase):
    def probe(self,fault=None):
        with tempfile.TemporaryDirectory(dir=os.environ['TMPDIR']) as directory:
            role=pathlib.Path(directory);output=role/'run-synthetic'
            args=argparse.Namespace(role='builder',output=str(output),wrk='fake')
            tool={'sha256':'elf','libraries':{'lib':'hash'},'version':'fixed','version_exit':1,'ldd':'addr1'}
            final=dict(tool,ldd='addr2')
            if fault=='library':final['libraries']={'lib':'changed'}
            if fault=='binary':final['sha256']='changed'
            identities=[{'script':'hash'},{'script':'changed' if fault=='script' else 'hash'}]
            calls={'small':0}
            def render_sample(*args):
                row=sample(*args)
                if fault=='noise' and row['label']=='C' and row['payload']['size']==1024:
                    row['measurement']['qps']=(50,100,150)[calls['small']]
                    calls['small']+=1
                if fault=='large-threshold' and row['label']=='D' and row['payload']['size']==1048576:
                    row['measurement']['latency_ms']['p99']=200
                return row
            with ExitStack() as patches:
                patches.enter_context(mock.patch.object(repair,'role_root',return_value=role))
                patches.enter_context(mock.patch.object(repair.bench,'environment',return_value={'kind':'synthetic'}))
                patches.enter_context(mock.patch.object(repair,'validate_manifest',side_effect=lambda path,label:{'label':label}))
                patches.enter_context(mock.patch.object(repair.bench,'validate_tool',side_effect=[tool,final]))
                patches.enter_context(mock.patch.object(repair,'tool_identity',side_effect=identities))
                patches.enter_context(mock.patch.object(repair.bench,'run_sample',side_effect=repair.bench.Invalid('audit failure') if fault=='audit' else render_sample))
                if fault=='budget':patches.enter_context(mock.patch.object(repair,'budget_seconds',return_value=1801))
                if fault=='log':patches.enter_context(mock.patch.object(repair,'log_bytes',return_value=repair.bench.LOG_LIMIT+1))
                code=repair.run_suite(args)
            result=json.loads((output/'run.json').read_text())
            self.assertFalse((output/'root').exists())
            self.assertEqual(code,1 if fault else 0)
            self.assertEqual(result['status'],'invalid' if fault else 'valid')
            if fault:self.assertNotIn('summary',result)

    def test_direct_output_only(self):
        with tempfile.TemporaryDirectory(dir=os.environ['TMPDIR']) as directory:
            root=pathlib.Path(directory)
            with mock.patch.object(repair,'role_root',return_value=root):
                self.assertEqual(repair.new_output('builder',root/'run-direct'),root/'run-direct')
                with self.assertRaisesRegex(repair.bench.Invalid,'role run- output required'):
                    repair.new_output('builder',root/'tmp/run-nested')
            self.assertFalse((root/'tmp').exists())

    def test_historical_nested_budget(self):
        with tempfile.TemporaryDirectory(dir=os.environ['TMPDIR']) as directory:
            root=pathlib.Path(directory)
            nested=root/'tmp/run-old';nested.mkdir(parents=True)
            (nested/'run.json').write_text(json.dumps({'kind':'formal','wall_seconds':42}))
            self.assertEqual(repair.budget_seconds(root),42)

    def test_group_budget_uses_records_not_names(self):
        with tempfile.TemporaryDirectory(dir=os.environ['TMPDIR']) as directory:
            root=pathlib.Path(directory)
            for index,kind in enumerate(('trace','experiment'),1):
                path=root/f'run-other-{index}';path.mkdir()
                (path/'run.json').write_text(json.dumps({'kind':kind,'wall_seconds':1}))
            repair.require_mechanism_budget(root)
            third=root/'tmp/run-other-3';third.mkdir(parents=True)
            (third/'run.json').write_text(json.dumps({'kind':'trace','wall_seconds':1,'status':'invalid'}))
            with self.assertRaisesRegex(repair.bench.Invalid,'two mechanism group limit'):
                repair.require_mechanism_budget(root)

    def test_stable_aslr(self):self.probe()
    def test_library_drift(self):self.probe('library')
    def test_binary_drift(self):self.probe('binary')
    def test_script_drift(self):self.probe('script')
    def test_audit_failure(self):self.probe('audit')
    def test_role_budget(self):self.probe('budget')
    def test_log_budget(self):self.probe('log')

    def test_noisy_suite_invalid(self):self.probe('noise')
    def test_large_threshold_invalid(self):self.probe('large-threshold')

    def test_mechanism_deadline(self):
        with mock.patch.object(mechanism.socket,'create_connection'):
            with self.assertRaisesRegex(repair.bench.Invalid,'10 second group budget'):
                mechanism.receive_series(1,{},0)

    def test_snapshot_drift(self):
        with mock.patch.object(repair.bench,'validate_manifest',return_value={'base_commit':repair.BASE,'workspace_hashes':{'x':'old'}}),mock.patch.object(repair,'workspace_hashes',return_value={'x':'new'}):
            with self.assertRaisesRegex(repair.bench.Invalid,'workspace identity drift'):
                repair.validate_manifest('synthetic','D')

if __name__=='__main__':unittest.main()
