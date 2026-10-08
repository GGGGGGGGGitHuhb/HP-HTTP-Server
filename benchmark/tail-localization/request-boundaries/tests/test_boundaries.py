"""Actual decoder wire fixtures; histogram validation belongs to smoke/decode slots."""
import importlib.util
import json
from pathlib import Path
import struct
import sys
import unittest
sys.dont_write_bytecode = True


def load_decoder(root):
    spec = importlib.util.spec_from_file_location('r018_fixture_decoder', root/'decode_boundaries.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def write_json(path, value):
    path.write_text(json.dumps(value, allow_nan=False)+'\n')


def fixture(root):
    root.mkdir(exist_ok=False)
    clock = {'clock':'CLOCK_MONOTONIC','clock_resolution_ns':1,'boot_id':'fixture-boot','time_namespace':'fixture-time'}
    processes = [{'name':name,'identity':{'pid':pid,'starttime':42},'clock_identity':dict(clock)}
                 for name,pid in [('server',101),('client',102)]]
    sample = {'run_id':'fixture','mode':'B','request_bytes':96,'processes':processes}
    def metadata(pid):
        def observation(now):
            return {'clock':'CLOCK_MONOTONIC','resolution_ns':1,'process_starttime':42,
                    'boot_id':'fixture-boot','time_namespace':'fixture-time','observed_ns':now}
        return {'schema':'baseline-v1-map','run_id':'fixture','pid':pid,'process_starttime':42,
                'clock_start':observation(10),'clock_stop':observation(1000)}
    client = {'warm_start_ns':100,'T0_ns':100,'T1_ns':1000,'threads':[
        {'owner':owner,'completed':[64,0,0,0],'connections':[],'slow_records':[]} for owner in range(2)]}
    mapping = {**metadata(102),'connections':[]}
    workers = []
    for worker in range(4):
        row = {**metadata(101),'schema':'request-boundaries-worker-v1','worker':worker,'tid':201+worker,'record_count':32,
               'completed':32,'incomplete':0,'overflow':False,'writer_stopped':True,'invalid_reason':'','connections':[]}
        for connection_id in range(1,33):
            number = worker*32+connection_id-1
            owner, life = number//64, number%64+1
            address = {'client_address_u32':0x7f000001,'client_port':20000+number,
                       'server_address_u32':0x7f000001,'server_port':8080}
            client['threads'][owner]['connections'].append({'life':life,'sequence':1,'ready_ns':50,
                'active':False,'full_sent':True,'sent_bytes':96,'end_reason':3})
            mapping['connections'].append({**address,'owner':owner,'life':life,'fd':100+number,'connected_ns':50,'final_sequence':1})
            row['connections'].append({**address,'connection_id':connection_id,'fd':300+number,
                'opened_ns':10,'closed_ns':1000,'last_sequence':1,'partial_eof':False,'tail_request_parsed':True})
        workers.append(row)
    client['threads'][0]['slow_records'].append({'life':1,'sequence':1,'class':1,'status':200,
        'body_bytes':1024,'start_ns':110,'complete_ns':300})
    write_json(root/'sample.json',sample)
    write_json(root/'client.json',client)
    write_json(root/'client-map.json',mapping)
    for row in workers:
        worker=row['worker']
        write_json(root/f'boundary-worker-{worker}.json',row)
        header=struct.pack('<8sII10Q',b'HPBOUND1',1,worker,101,201+worker,42,32,32,0,0,1,1,0)
        records=b''.join(struct.pack('<IIQQQ',identity,3,1,120,200) for identity in range(1,33))
        (root/f'boundary-worker-{worker}.bin').write_bytes(header+records+b'HPBTAIL1'+header[8:])
    return root


def create_suite(package, scratch):
    decoder=load_decoder(Path(package))
    class BoundaryTests(unittest.TestCase):
        def setUp(self):
            self.root=fixture(Path(scratch)/self._testMethodName)
        def decode(self):
            return decoder.decode_sample(self.root,verify_statistics=False)
        def test_complete_wire(self):
            result=self.decode()
            self.assertEqual((result['slow_count'],result['ordered_count'],len(result['connection_coverage'])),(1,1,128))
            self.assertEqual(result['records'][0]['signed_intervals_ns'],[10,80,100])
        def test_crossed_signed_boundary(self):
            path=self.root/'boundary-worker-0.bin';wire=bytearray(path.read_bytes())
            struct.pack_into('<Q',wire,96+24,400);path.write_bytes(wire)
            record=self.decode()['records'][0]
            self.assertFalse(record['ordered'])
            self.assertEqual(record['signed_intervals_ns'],[10,280,-100])
            self.assertIsNone(record['longest_interval'])
        def test_trailer_and_partial_record(self):
            path=self.root/'boundary-worker-0.bin';wire=path.read_bytes()
            path.write_bytes(wire[:-1])
            with self.assertRaisesRegex(ValueError,'wire trailer'):self.decode()
            path.write_bytes(wire[:96+31])
            with self.assertRaisesRegex(ValueError,'partial record'):self.decode()
        def test_duplicate_tuple(self):
            path=self.root/'client-map.json';mapping=json.loads(path.read_text())
            mapping['connections'][1]['client_port']=mapping['connections'][0]['client_port'];write_json(path,mapping)
            with self.assertRaisesRegex(ValueError,'duplicate client tuple'):self.decode()
        def test_partial_eof_last_slot(self):
            client_path=self.root/'client.json';client=json.loads(client_path.read_text())
            state=client['threads'][0]['connections'][1]
            state.update(active=True,full_sent=False,sent_bytes=1,end_reason=1)
            client['threads'][0]['completed'][0]=63;write_json(client_path,client)
            metadata_path=self.root/'boundary-worker-0.json';metadata=json.loads(metadata_path.read_text())
            metadata.update(completed=31,incomplete=1)
            metadata['connections'][1].update(partial_eof=True,tail_request_parsed=False);write_json(metadata_path,metadata)
            path=self.root/'boundary-worker-0.bin';wire=bytearray(path.read_bytes())
            struct.pack_into('<Q',wire,48,31);struct.pack_into('<Q',wire,56,1)
            struct.pack_into('<IIQQQ',wire,96+32,2,1,1,120,0)
            wire[-88:]=wire[8:96];path.write_bytes(wire)
            result=self.decode()
            self.assertEqual(result['connection_coverage'][1]['incomplete'],1)
            metadata['connections'][1]['tail_request_parsed']=True;write_json(metadata_path,metadata)
            with self.assertRaisesRegex(ValueError,'partial EOF'):self.decode()
        def test_overflow_wire_status(self):
            path=self.root/'boundary-worker-0.bin';wire=bytearray(path.read_bytes())
            struct.pack_into('<Q',wire,64,1);wire[-88:]=wire[8:96];path.write_bytes(wire)
            with self.assertRaisesRegex(ValueError,'wire header status'):self.decode()
    return unittest.defaultTestLoader.loadTestsFromTestCase(BoundaryTests)
