"""New exact fixture guard and scoped binary decoder cases, no live sockets."""
import unittest
from test_r008_guard import R008FixtureBoundaryTests
from test_r008_decoder import ScopedBinaryCounterexamples,IdentityAndQuotaTests

if __name__=='__main__':
    suite=unittest.TestSuite(unittest.defaultTestLoader.loadTestsFromTestCase(case)
        for case in (R008FixtureBoundaryTests,ScopedBinaryCounterexamples,IdentityAndQuotaTests))
    result=unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)
