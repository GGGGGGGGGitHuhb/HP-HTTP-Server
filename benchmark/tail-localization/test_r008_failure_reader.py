"""Reader observes only fully published fixed slots; no sockets or private resources."""
import ctypes
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch
from control_protocol_v3 import read_published_failure
from wire_types_v3 import Control

class FailureReaderTests(unittest.TestCase):
    def make_control(self,directory,published):
        control=Control();control.magic=b'S4CTRL03';control.version=3;control.bytes=ctypes.sizeof(Control)
        control.abortRun=1;control.firstFailure.published=published
        control.firstFailure.code=99
        path=Path(directory)/'control.bin';path.write_bytes(bytes(control))
        return path
    def test_empty_slot_remains_unknown(self):
        with tempfile.TemporaryDirectory() as directory:
            path=self.make_control(directory,0)
            self.assertEqual(read_published_failure(path,time.monotonic()+1),'unknown')
    def test_partial_writer_exit_never_exposes_fields(self):
        with tempfile.TemporaryDirectory() as directory:
            path=self.make_control(directory,1)
            self.assertEqual(read_published_failure(path,time.monotonic()+1),'unknown')
            self.assertEqual(Control.from_buffer_copy(path.read_bytes()).firstFailure.published,1)
    def test_abort_arrives_before_publish_reader_waits(self):
        with tempfile.TemporaryDirectory() as directory:
            path=self.make_control(directory,1)
            class PublishedAfterFirstLoad:
                def __init__(self):self.loads=0
                def load(self,structure,field):
                    self.loads+=1
                    if self.loads==2:
                        structure.code=3;structure.savedErrno=123;structure.connection.fd=7
                        structure.published=2
                    return structure.published
            with patch('wire_types_v3.Atomics',PublishedAfterFirstLoad):
                result=read_published_failure(path,time.monotonic()+1)
            self.assertEqual((result['published'],result['code'],result['savedErrno'],result['connection']['fd']),(2,3,123,7))
    def test_expired_deadline_does_not_wait_for_dead_writer(self):
        with tempfile.TemporaryDirectory() as directory:
            path=self.make_control(directory,1)
            self.assertEqual(read_published_failure(path,time.monotonic()-1),'unknown')

if __name__=='__main__':unittest.main()
