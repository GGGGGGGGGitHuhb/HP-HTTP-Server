"""Exactly current M2 obligations; parent owns the single charged 20s invocation."""
import unittest
from test_localize import TimelineTests
from test_r009_payload import PayloadContractTests
from test_r009_m2 import CurrentMetadataTests
if __name__=='__main__':
    suite=unittest.TestSuite(unittest.defaultTestLoader.loadTestsFromTestCase(case) for case in (TimelineTests,PayloadContractTests,CurrentMetadataTests))
    result=unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)
