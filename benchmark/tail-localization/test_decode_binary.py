"""Pure schema-2 fixtures; no sockets or live processes. Requires admitted execution."""
import json
from pathlib import Path
import tempfile
import unittest
from analyze import Invalid
from decode_v2 import decode, HEADER, RECORD


def event(time,kind,value=0,flags=0,result=0):
    return (time,value,0,1,kind,flags,result)


def fixture(directory,server,client):
    mappings=[]
    for endpoint,count,pid in [('server',4,100),('client',2,200)]:
        for worker in range(count):
            mappings.append(dict(endpoint=endpoint,role='worker',worker=worker,pid=pid,
                namespace_tid=pid+worker+1,kernel_tid=pid+worker+1000,starttime=10+worker))
        mappings.append(dict(endpoint=endpoint,role='main',worker=count,pid=pid,
            namespace_tid=pid,kernel_tid=pid+1000,starttime=10,
            identity=dict(boot_id='fixture-boot',time_namespace='time:[1]')))
    pair=dict(four_tuple=[2130706433,3000,2130706433,4000],
              server=dict(pid=100,tid=101,worker=0,starttime=10,index=0,lifetime=1),
              client=dict(pid=200,tid=201,worker=0,starttime=10,index=0,lifetime=1))
    sample=dict(status='valid',cleanup=dict(forced=False,errors=[],remaining=[]),
        parameters=dict(detailed=True),kernel_mappings=mappings,frozen_connections=[pair],
        recording_start_ns=1,recording_end_ns=10000,measurement_start_ns=50,measurement_end_ns=5000,
        connection_end_states=[dict(endpoint='client',index=0,reserved=int(bool(client)))],
        final_threads=[dict(endpoint='client',worker=0,stoppedNs=10000)])
    (directory/'sample.json').write_text(json.dumps(sample))
    (directory/'marker-launcher.json').write_text(json.dumps(dict(status='valid',marker_lost_events=0)))
    (directory/'processes.json').write_text(json.dumps([dict(name='server',starttime=5),dict(name='client',starttime=6)]))
    for endpoint,count in [('server',4),('client',2)]:
        for worker in range(count):
            mapping=next(m for m in mappings if m['endpoint']==endpoint and m['worker']==worker)
            records=sorted((server if endpoint=='server' else client) if worker==0 else [])
            first,last=(records[0][0],records[-1][0]) if records else (0,0)
            header=HEADER.pack(b'S4TAIL02',64,32,1,len(records),16*1024*1024//32,
                               mapping['pid'],mapping['namespace_tid'],mapping['starttime'],first,last)
            (directory/f'{endpoint}-worker-{worker}.events.bin').write_bytes(header+b''.join(RECORD.pack(*row) for row in records))


def complete_events():
    server=[event(104,5,1),event(105,6,8192),event(106,7,64),event(107,17),
            event(108,8,64),event(109,9,69),event(110,10,1093),event(111,18),
            event(112,11,69),event(113,12,69),event(114,13,1024),event(115,14,1024),
            event(116,15),event(122,16,1)]
    client=[event(100,1,64),event(101,19,64),event(102,20,64),event(103,2,64),
            event(117,21,8192),event(118,22,1093),event(119,3,1093),event(120,4,1024,1,200)]
    return server,client


class BinaryCounterexamples(unittest.TestCase):
    def evaluate(self,server,client):
        with tempfile.TemporaryDirectory(prefix='decode-fixture-') as name:
            directory=Path(name);fixture(directory,server,client)
            return decode(directory)

    def test_complete_before_outer_return_is_valid(self):
        result=self.evaluate(*complete_events())
        self.assertEqual(result['complete_requests'],1)

    def test_idle_before_and_after_client_origin(self):
        for begin in (80,100):
            server,client=complete_events()
            server += [event(begin,5,1),event(begin+1,6,8192),event(begin+2,7,0,1,11),event(begin+3,16,1)]
            result=self.evaluate(server,client)
            self.assertEqual(result['complete_requests'],1)
            self.assertEqual(result['idle_reasons']['fully_paired_prospective_scope_before_real_origin'],2)

    def test_idle_internal_recv_missing_is_invalid(self):
        idle=[event(80,5,1),event(81,6,8192),event(83,16,1)]
        with self.assertRaises(Invalid):self.evaluate(idle,[])

    def test_fully_paired_idle_has_no_request(self):
        idle=[event(80,5,1),event(81,6,8192),event(82,7,0,1,11),event(83,16,1)]
        result=self.evaluate(idle,[])
        self.assertEqual(result['complete_requests'],0)
        self.assertEqual(result['idle_groups'],1)

    def test_partial_recv_and_send_multiple_callbacks_same_sequence(self):
        server,client=complete_events()
        client[:4]=[event(80,1,64),event(81,19,64),event(82,20,64),event(83,2,64)]
        # First callback receives a partial request and returns EAGAIN; sequence remains one.
        server=[event(90,5,1),event(91,6,8192),event(92,7,32),event(93,6,8192),
                event(94,7,0,1,11),event(95,16,1)]+server
        server[8]=event(106,7,32)
        # Header short write, retry, then remainder: successful bytes still sum to 1093.
        server=[(row[0]*10,*row[1:]) for row in server if row[4] not in (11,12)]
        client=[(row[0]*10,*row[1:]) for row in client]
        server += [event(1120,11,69),event(1121,12,20),event(1122,11,49),
                   event(1123,12,0,1,11),event(1124,11,49),event(1125,12,49)]
        result=self.evaluate(server,client)
        self.assertEqual(result['complete_requests'],1)

    def test_return_over_request_and_unknown_flags_invalid(self):
        for replacement in (event(106,7,8193),event(106,7,64,4,0)):
            server,client=complete_events();server[2]=replacement
            with self.assertRaises(Invalid):self.evaluate(server,client)


if __name__=='__main__':unittest.main()
