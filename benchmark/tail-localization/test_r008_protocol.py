"""Synthetic bounded protocol counterexamples; mock transport, no socket probes."""
import json
import socket
import time
import unittest
from control_protocol_v3 import encode_message,receive_message,MAX_MESSAGE

class SyntheticChannel:
    def __init__(self, raw, flags=0): self.raw=raw;self.flags=flags
    def settimeout(self,value): self.timeout=value
    def recvmsg(self,limit): return self.raw,[],self.flags,None

class StartupProtocolTests(unittest.TestCase):
    def parse(self,raw,flags=0,seen=None,kind='ready'):
        return receive_message(SyntheticChannel(raw,flags),'run-r008-test',kind,time.monotonic()+1,seen or set())
    def test_valid_ready(self):
        self.assertEqual(self.parse(encode_message('run-r008-test','frozen','ready'))['schema'],3)
    def test_eof_is_peer_closed(self):
        with self.assertRaisesRegex(EOFError,'peerClosedBeforeReady'):self.parse(b'')
    def test_truncated(self):
        with self.assertRaisesRegex(ValueError,'truncated'):self.parse(b'{}',socket.MSG_TRUNC)
    def test_malformed(self):
        with self.assertRaises(ValueError):self.parse(b'{')
    def test_schema_and_run(self):
        for payload in (dict(schema=2,runId='run-r008-test',kind='ready',phase='frozen'),dict(schema=3,runId='other',kind='ready',phase='frozen')):
            with self.assertRaises(ValueError):self.parse(json.dumps(payload).encode())
    def test_phase(self):
        with self.assertRaises(ValueError):self.parse(json.dumps(dict(schema=3,runId='run-r008-test',kind='ready',phase='running')).encode())
    def test_duplicate_ready_go(self):
        for phase,kind in [('frozen','ready'),('running','go')]:
            with self.assertRaisesRegex(ValueError,'duplicate'):self.parse(encode_message('run-r008-test',phase,kind),seen={kind},kind=kind)
    def test_abort_preserves_published_first_cause(self):
        with self.assertRaisesRegex(RuntimeError,'connectionLimitExceeded'):
            self.parse(encode_message('run-r008-test','aborted','abort',first_failure='connectionLimitExceeded'))
    def test_ready_after_abort(self):
        with self.assertRaises(ValueError):self.parse(encode_message('run-r008-test','frozen','ready'),seen={'abort'})
    def test_size_and_absolute_deadline(self):
        with self.assertRaises(ValueError):encode_message('run-r008-test','frozen','ready',padding='x'*MAX_MESSAGE)
        with self.assertRaises(TimeoutError):receive_message(SyntheticChannel(b'{}'),'run-r008-test','ready',time.monotonic()-1,set())

if __name__=='__main__':unittest.main()
