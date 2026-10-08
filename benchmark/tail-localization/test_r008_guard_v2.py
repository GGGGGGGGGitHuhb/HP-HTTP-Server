"""Mock metadata for real guard classifiers; never place symlinks in role TMP."""
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
import unittest
from budget_v9 import BudgetError,file_bytes

class R008FixtureBoundaryTests(unittest.TestCase):
    def fixture(self):
        run=Path('/mock-stage/builder/run-r008-build-002')
        fixture=run/'server/.test-tmp';case=fixture/'static-Abc123'
        return run,fixture,case
    def count(self,run,fixture,entries):
        def mock_scan(root,classify,allow_missing,disappearance):
            self.assertEqual(root,run)
            return sum(classify(str(path.relative_to(run)),path,SimpleNamespace(st_size=size),target) for path,size,target in entries)
        with patch('budget_v9.scan_bytes',side_effect=mock_scan):
            return file_bytes(run,fixture_roots=(fixture,))
    def test_exact_source_links_count_without_following(self):
        run,fixture,case=self.fixture()
        links=[(case/'root/escape.txt',str(case/'sibling-secret.txt')),(case/'root/escape-dir',str(case)),(case/'root-link',str(case/'root'))]
        entries=[(case/'sibling-secret.txt',6,None)]+[(path,len(target.encode()),target) for path,target in links]
        self.assertEqual(self.count(run,fixture,entries),6+sum(len(target.encode()) for path,target in links))
    def test_full_run_is_not_fixture_scope(self):
        run,fixture,case=self.fixture()
        with self.assertRaises(BudgetError):self.count(run,fixture,[(run/'raw.stdout',30,str(case/'sibling-secret.txt'))])
    def test_wrong_fixture_leaf_or_target_rejected(self):
        run,fixture,case=self.fixture()
        for suffix,target in [('root/other',case/'sibling-secret.txt'),('root/escape.txt',Path('/mock-stage/outside'))]:
            with self.assertRaises(BudgetError):self.count(run,fixture,[(case/suffix,len(str(target)),str(target))])
    def test_no_extra_recursive_count(self):
        run,fixture,case=self.fixture()
        self.assertEqual(self.count(run,fixture,[(case/'sibling-secret.txt',6,None),(run/'normal',3,None)]),9)

if __name__=='__main__':unittest.main()
