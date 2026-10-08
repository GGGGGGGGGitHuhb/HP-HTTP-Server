"""Actual Reservation admission against synthetic ordinary files; no links/socket."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from budget_v10 import Reservation,BudgetError

class ReceiptAdmissionTests(unittest.TestCase):
    def exercise(self,change=None,missing=False):
        with tempfile.TemporaryDirectory(prefix='r008-receipt-') as name:
            root=Path(name);(root/'leader').mkdir();(root/'builder').mkdir()
            slots=['r008-link-once','r008-link-repeat','r008-link-mapped','r008-link-128']
            auth=dict(roles_serial=True,per_role_dynamic_seconds=1200,per_role_output_bytes=2**31,builder_http_limits={},old_s3_ledgers=dict(builder='synthetic'),r008=dict(builder_subbudget_seconds=360,builder_m3_reserved_seconds=570,per_role_samples=slots,sample_outer_seconds=30))
            (root/'leader/authorization.json').write_text(json.dumps(auth))
            prior=root/'builder/run-r008-link-once';prior.mkdir()
            cleanup=dict(forced=False,errors=[],remaining=[])
            sample=dict(status='valid',manifest=dict(role='builder'),parameters=dict(connections=2,requests_per_connection=1,detailed=True),identity_scope='process-local',frozen_connections=[{},{}],cleanup=cleanup)
            rawsample=json.dumps(sample).encode();(prior/'sample.json').write_bytes(rawsample)
            catalog=[]
            for endpoint,total in [('server',4),('client',2)]:
                for worker in range(total):
                    raw=prior/f'{endpoint}-worker-{worker}.events.bin';raw.write_bytes(b'raw')
                    catalog.append(dict(path=str(raw),sha256=hashlib.sha256(b'raw').hexdigest(),records=1,identity_scope='process-local'))
            receipt=dict(schema=3,status='valid',run_id=prior.name,role='builder',sample_sha256=hashlib.sha256(rawsample).hexdigest(),manifest_sha256=hashlib.sha256(json.dumps(sample['manifest'],sort_keys=True,separators=(',',':')).encode()).hexdigest(),parameters=sample['parameters'],identity_scope='process-local',kernel_mapping_verified=False,selected_connections=2,overflow=False,cleanup=cleanup,complete_requests=2,boundaries=[],file_catalog=catalog)
            if change:change(receipt)
            (prior/'association.json').write_text(json.dumps(receipt))
            if missing:(prior/'association.json').unlink()
            (root/'builder/ledger.json').write_text(json.dumps(dict(schema=1,role='builder',runs=[dict(run_id=prior.name,kind='r008_link',status='valid',reserved_seconds=30,charged_seconds=1)])))
            with Reservation(root,'builder','run-r008-link-repeat','r008_link',30):pass
    def test_bound_receipt_passes(self):self.exercise()
    def test_wrong_binding_rejected(self):
        for key,value in [('run_id','other'),('role','reviewer'),('sample_sha256','bad'),('manifest_sha256','bad'),('parameters',{}),('identity_scope','kernel-mapped'),('selected_connections',1),('complete_requests',1),('overflow',True),('cleanup',{})]:
            with self.subTest(key=key),self.assertRaises(BudgetError):self.exercise(lambda r:r.update({key:value}))
    def test_raw_catalog_rejected(self):
        for mutation in [lambda r:r['file_catalog'][0].update(sha256='bad'),lambda r:r['file_catalog'][0].update(path=r['file_catalog'][0]['path'].replace('server-worker-0','../server-worker-0')),lambda r:r['file_catalog'].pop(),lambda r:r.update(boundaries=[{}])]:
            with self.assertRaises(BudgetError):self.exercise(mutation)

    def test_missing_receipt_rejected(self):
        with self.assertRaises(BudgetError):self.exercise(missing=True)
