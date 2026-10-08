"""Schema-3 identity scopes inherit complete binary counterexamples without sockets."""
import json
from pathlib import Path
import tempfile
import unittest
from analyze import Invalid
from decode_v3 import decode
from test_decode_binary import BinaryCounterexamples,complete_events,fixture


def scope_fixture(directory,server,client,mapped=False,quota=0):
    fixture(directory,server,client)
    sample=json.loads((directory/'sample.json').read_text())
    sample.update(manifest=dict(role='builder'),schema=3,identity_scope='kernel-mapped' if mapped else 'process-local',kernel_mapping_verified=mapped)
    sample['parameters'].update(connections=1,requests_per_connection=quota)
    if not mapped:
        mappings=sample.pop('kernel_mappings')
        for item in mappings:item.pop('kernel_tid')
        sample['process_local_mappings']=mappings
        (directory/'marker-launcher.json').unlink()
    (directory/'sample.json').write_text(json.dumps(sample))

class ScopedBinaryCounterexamples(BinaryCounterexamples):
    def evaluate(self,server,client):
        with tempfile.TemporaryDirectory(prefix='r008-decode-fixture-') as name:
            directory=Path(name);scope_fixture(directory,server,client)
            result=decode(directory)
            self.assertEqual(result['identity_scope'],'process-local')
            self.assertFalse(result['kernel_mapping_verified'])
            self.assertTrue(all(item['kernel_tid'] is None for item in result['file_catalog']))
            return result

class IdentityAndQuotaTests(unittest.TestCase):
    def test_kernel_mapping_flag_cannot_be_faked(self):
        with tempfile.TemporaryDirectory() as name:
            directory=Path(name);scope_fixture(directory,*complete_events())
            sample=json.loads((directory/'sample.json').read_text());sample['kernel_mapping_verified']=True
            (directory/'sample.json').write_text(json.dumps(sample))
            with self.assertRaises(Invalid):decode(directory)
    def test_process_mapping_cannot_smuggle_kernel_tid(self):
        with tempfile.TemporaryDirectory() as name:
            directory=Path(name);scope_fixture(directory,*complete_events())
            sample=json.loads((directory/'sample.json').read_text());sample['process_local_mappings'][0]['kernel_tid']=999
            (directory/'sample.json').write_text(json.dumps(sample))
            with self.assertRaises(Invalid):decode(directory)
    def test_fixed_target_exact_and_excess(self):
        for quota,valid in [(1,True),(2,False)]:
            with tempfile.TemporaryDirectory() as name:
                directory=Path(name);scope_fixture(directory,*complete_events(),quota=quota)
                if valid:self.assertEqual(decode(directory)['complete_requests'],1)
                else:
                    with self.assertRaises(Invalid):decode(directory)
    def test_mapped_scope_requires_marker_summary(self):
        with tempfile.TemporaryDirectory() as name:
            directory=Path(name);scope_fixture(directory,*complete_events(),mapped=True)
            (directory/'marker-launcher.json').unlink()
            with self.assertRaises(FileNotFoundError):decode(directory)

if __name__=='__main__':unittest.main()
