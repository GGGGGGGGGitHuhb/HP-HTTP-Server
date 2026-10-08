"""Current decode_v3 metadata failures, without sockets or altered old fixtures."""
import json
from pathlib import Path
import tempfile
import unittest
from analyze import Invalid
from decode_v3 import decode,HEADER
from test_decode_binary import complete_events
from test_r008_decoder_v2 import scope_fixture

class CurrentMetadataTests(unittest.TestCase):
    def reject(self,mutation):
        with tempfile.TemporaryDirectory(prefix='r009-m2-fixture-') as name:
            directory=Path(name);scope_fixture(directory,*complete_events(),quota=1)
            mutation(directory)
            with self.assertRaises(Invalid):decode(directory)
    def test_header_overflow(self):
        def change(directory):
            p=directory/'server-worker-0.events.bin';b=p.read_bytes();h=list(HEADER.unpack(b[:64]));h[3]=3;p.write_bytes(HEADER.pack(*h)+b[64:])
        self.reject(change)
    def test_owner_mismatch(self):
        def change(directory):
            p=directory/'server-worker-0.events.bin';b=p.read_bytes();h=list(HEADER.unpack(b[:64]));h[6]+=1;p.write_bytes(HEADER.pack(*h)+b[64:])
        self.reject(change)
    def test_record_length(self):
        def change(directory):
            p=directory/'server-worker-0.events.bin';p.write_bytes(p.read_bytes()+b'x')
        self.reject(change)
    def test_clock_drift(self):
        def change(directory):
            p=directory/'sample.json';s=json.loads(p.read_text())
            for item in s['process_local_mappings']:
                if item['endpoint']=='client' and item['role']=='main':item['identity']['boot_id']='different-boot'
            p.write_text(json.dumps(s))
        self.reject(change)
