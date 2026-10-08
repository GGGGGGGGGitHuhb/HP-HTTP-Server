"""R008 exact source fixture guard counterexamples; no socket probes."""
from pathlib import Path
import tempfile
import unittest
from budget_v9 import BudgetError, file_bytes

class R008FixtureBoundaryTests(unittest.TestCase):
    def fixture(self,directory):
        run=Path(directory)/'run-r008-build-002'
        fixture=run/'server/.test-tmp';case=fixture/'static-Abc123'
        (case/'root').mkdir(parents=True)
        (case/'sibling-secret.txt').write_bytes(b'secret')
        return run,fixture,case
    def test_exact_source_links_count_without_following(self):
        with tempfile.TemporaryDirectory() as directory:
            run,fixture,case=self.fixture(directory)
            links=[case/'root/escape.txt',case/'root/escape-dir',case/'root-link']
            links[0].symlink_to(case/'sibling-secret.txt');links[1].symlink_to(case);links[2].symlink_to(case/'root')
            self.assertEqual(file_bytes(run,fixture_roots=(fixture,)),6+sum(link.lstat().st_size for link in links))
    def test_full_run_is_not_fixture_scope(self):
        with tempfile.TemporaryDirectory() as directory:
            run,fixture,case=self.fixture(directory)
            (run/'raw.stdout').symlink_to(case/'sibling-secret.txt')
            with self.assertRaises(BudgetError):file_bytes(run,fixture_roots=(fixture,))
    def test_wrong_fixture_leaf_or_target_rejected(self):
        for suffix,target in [('root/other', 'sibling-secret.txt'),('root/escape.txt','../../../outside')]:
            with tempfile.TemporaryDirectory() as directory:
                run,fixture,case=self.fixture(directory);(case/suffix).symlink_to(case/target)
                with self.assertRaises(BudgetError):file_bytes(run,fixture_roots=(fixture,))
    def test_no_extra_recursive_count(self):
        with tempfile.TemporaryDirectory() as directory:
            run,fixture,case=self.fixture(directory)
            (run/'normal').write_bytes(b'abc')
            self.assertEqual(file_bytes(run,fixture_roots=(fixture,)),9)

if __name__=='__main__':unittest.main()
