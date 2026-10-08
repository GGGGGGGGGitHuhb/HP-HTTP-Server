"""Deterministic wire-contract counterexamples; execution requires admission."""
import errno
import unittest
from analyze import Invalid
from decode_v3 import validate_payload


class PayloadContractTests(unittest.TestCase):
    def test_verified_body(self):
        validate_payload(4,1024,1,200)
        for payload in ((1023,1,200),(1024,0,200),(1024,1,201)):
            with self.assertRaises(Invalid):validate_payload(4,*payload)

    def test_server_io(self):
        validate_payload(7,1024,0,0)
        validate_payload(7,0,1,errno.EAGAIN)
        for payload in ((0,2,errno.EAGAIN),(3,1,errno.EAGAIN),(0,1,-1),(3,0,errno.EIO)):
            with self.assertRaises(Invalid):validate_payload(7,*payload)

    def test_client_retry(self):
        validate_payload(22,0,2,errno.EAGAIN)
        with self.assertRaises(Invalid):validate_payload(22,0,2,errno.EIO)

    def test_scope_and_enter(self):
        validate_payload(17,0,0,0)
        validate_payload(6,4096,0,0)
        for args in ((17,1,0,0),(6,4096,0,1),(6,0,0,0),(5,32,0,0)):
            with self.assertRaises(Invalid):validate_payload(*args)


if __name__=='__main__':unittest.main()
