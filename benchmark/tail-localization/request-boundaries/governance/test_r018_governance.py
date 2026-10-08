"""真实Reservation正常结算→R018真实consumer；外部计时/扫描接缝隔离。"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
sys.dont_write_bytecode = True
TOOL = Path('/home/power/projects/HP-HTTP-Server/benchmark/tail-localization')
CLOSURE = [TOOL / name for name in ('budget_v13.py', 'localize_v15.py', 'r018_admission.py',
    'protection.py', 'proc_identity_v11.py', 'cleanup_v11.py', 'watchdog_v11.py', 'r016/r015_admission.py', 'r016/__init__.py')]
for source in [Path(__file__), Path(__file__).with_name('r018_admission.py'), *CLOSURE]:
    compile(source.read_bytes(), str(source), 'exec')


def load(name, path, package=False):
    spec = importlib.util.spec_from_file_location(name, path, submodule_search_locations=[str(path.parent)] if package else None)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module

load('r016', TOOL / 'r016/__init__.py', True)
load('r016.r015_admission', TOOL / 'r016/r015_admission.py')
admission = load('r018_admission', Path(__file__).with_name('r018_admission.py'))
budget = load('budget_v13', TOOL / 'budget_v13.py')

class ContractTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='r018-contract-', dir=os.environ['TMPDIR'])
        self.addCleanup(self.temp.cleanup)
        self.stage = Path(self.temp.name)
        for role in ('builder', 'reviewer'):
            (self.stage / role).mkdir()
            (self.stage / role / 'ledger.json').write_text(json.dumps({'role': role, 'runs': []}))
        actual_authorization = json.loads((TOOL.parent.parent / '.cache/v0.5.1-s4/leader/authorization.json').read_text())
        self.authorization = actual_authorization
        self.debt_patch = patch.object(sys.modules['r016.r015_admission'], 'control_debt', return_value=60)
        self.debt_patch.start()
        self.addCleanup(self.debt_patch.stop)

    def produce(self):
        role = self.stage / 'builder'
        run = 'run-r018-build-001'
        output = role / run
        (output / 'build-output').mkdir(parents=True)
        original = Path(os.environ['HP_BASELINE_OUTPUT_ROOT']).parent / 'run-r018-build-001/build-output/build-receipt.json'
        (output / 'build-output/build-receipt.json').write_bytes(original.read_bytes())
        parser = argparse.ArgumentParser()
        parser.add_argument('--seconds', type=float)
        seconds = parser.parse_args(['--seconds', '90']).seconds
        producer = budget.Reservation(self.stage, 'builder', run, 'selfcheck', seconds)
        producer.started = 100.0
        producer.authorization = {'per_role_output_bytes': 2 * 1024**3}
        producer.output = output
        producer.ledger_path = role / 'ledger.json'
        producer.lock = (self.stage / 'fixture.lock').open('a+')
        producer.static_inventory = {}
        producer.record = {'run_id': run, 'kind': 'selfcheck', 'status': 'running', 'reserved_seconds': seconds, 'output': str(output)}
        producer.ledger = {'schema': 1, 'role': 'builder', 'runs': [producer.record]}
        admission.record_build_artifact(SimpleNamespace(run_id=run), producer,
            {'HP_BASELINE_PACKAGE_SHA256': os.environ['HP_BASELINE_PACKAGE_SHA256']})
        with patch.object(producer, 'output_bytes', return_value=1234), patch.object(producer, 'verify_static_hashes'), patch.object(budget.time, 'monotonic', return_value=101.0):
            producer.__exit__(None, None, None)
        (output / 'cleanup.json').write_text(json.dumps({'complete': True, 'forced': False, 'errors': [], 'remaining': [], 'unknown': []}))
        return json.loads(producer.ledger_path.read_text())['runs'][0]

    def replace(self, row, role='builder'):
        (self.stage / role / 'ledger.json').write_text(json.dumps({'role': role, 'runs': [row]}, allow_nan=True))

    def check(self, role='builder', run='run-r018-check-001', kind='ctest', seconds=60.0):
        return admission.check_slot(self.stage, role, run, kind, seconds, self.authorization)

    def test_actual_normal_settlement_and_consumer(self):
        row = self.produce()
        self.assertEqual(row['byte_classification_status'], 'verified')
        self.assertNotIn('byte_accounting', row)
        self.assertTrue(self.check())
        self.assertEqual(row['charged_seconds'], 1.0)

    def test_canonical_field_and_types(self):
        row = self.produce()
        wrong = dict(row)
        del wrong['byte_classification_status']
        wrong['byte_accounting'] = 'verified'
        self.replace(wrong)
        with self.assertRaises((KeyError, ValueError)): self.check()
        for key, value in [('status', 'invalid'), ('accounting_errors', {}), ('charged_seconds', True),
                           ('charged_seconds', float('nan')), ('reserved_seconds', 60), ('reserved_seconds', True), ('reserved_seconds', float('inf')), ('kind', 'offline'),
                           ('output', str(self.stage / 'reviewer/run-r018-build-001'))]:
            self.replace(dict(row, **{key: value}))
            with self.assertRaises(ValueError): self.check()

    def test_actual_artifact_sha_and_cleanup(self):
        row = self.produce()
        self.replace(dict(row, r018_build_receipt_sha256='0'*64))
        with self.assertRaises(ValueError): self.check()
        self.replace(row)
        path = self.stage / 'builder/run-r018-build-001/cleanup.json'
        path.write_text(json.dumps({'complete': False, 'forced': False, 'errors': [], 'remaining': [], 'unknown': []}))
        with self.assertRaises(ValueError): self.check()

    def test_exact_role_kind_duration_smoke_and_sequence(self):
        self.produce()
        for role, run, kind, seconds in [('builder','run-r018-boundary-01','boundary',45),
            ('builder','run-r018-check-001','selfcheck',60), ('builder','run-r018-check-001','ctest',61), ('builder','run-r018-check-001','ctest',True), ('builder','run-r018-check-001','ctest',60.5),
            ('builder','run-r018-smoke-002','smoke',20), ('reviewer','run-r018-boundary-03','boundary',45)]:
            with self.assertRaises((ValueError, KeyError)): self.check(role,run,kind,seconds)
        with self.assertRaises(ValueError): self.check(run='run-r018-smoke-001', kind='smoke', seconds=20)
        self.assertFalse(admission.check_slot(self.stage, 'builder', 'run-r017-check-001', 'selfcheck', 20, self.authorization))
        with self.assertRaises(ValueError): admission.check_slot(self.stage, 'reviewer', 'run-not-r018-001', 'boundary', 45, self.authorization)

    def test_frozen_current_role_build_record(self):
        root = Path(os.environ['HP_BASELINE_OUTPUT_ROOT']).parent
        ledger = json.loads((root / 'ledger.json').read_text())
        rows = [row for row in ledger['runs'] if row['run_id'] == 'run-r018-build-001']
        self.assertEqual(len(rows),1)
        row = rows[0]
        self.assertEqual(row['status'],'valid')
        self.assertEqual(row['byte_classification_status'],'verified')
        self.assertEqual(row['accounting_errors'],[])
        receipt = root / 'run-r018-build-001/build-output/build-receipt.json'
        self.assertEqual(row['r018_build_receipt_sha256'], hashlib.sha256(receipt.read_bytes()).hexdigest())
        self.assertEqual(row['r018_package_sha256'], os.environ['HP_BASELINE_PACKAGE_SHA256'])

if __name__ == '__main__':
    unittest.main()
