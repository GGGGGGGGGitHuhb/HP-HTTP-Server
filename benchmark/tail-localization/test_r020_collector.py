"""R019真实collector与实际Reservation接缝，隔离正式角色状态。"""
import copy
import hashlib
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import unittest
from unittest.mock import patch
import budget_v15 as budget
import localize_v17 as wrapper
import r020_admission as admission
from r019_capacity import measure_declared_root

REPOSITORY = Path(__file__).absolute().parents[2]
FORMAL = REPOSITORY / '.cache/v0.5.1-s4'


def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def write(path, value): path.write_text(json.dumps(value,indent=2)+'\n')

class RootTests(unittest.TestCase):
    def setUp(self):
        temporary=tempfile.TemporaryDirectory(prefix='r019-root-',dir=os.environ['TMPDIR']);self.addCleanup(temporary.cleanup)
        self.root=Path(temporary.name)
    def measure(self,path,future=False):
        return measure_declared_root(path,budget.scan_bytes,budget.fixture_link_bytes,(),future)
    def test_real_regular_directory_empty_future(self):
        (self.root/'shared.py').write_bytes(b'1234567');(self.root/'cache.json').write_bytes(b'12345');(self.root/'empty').touch()
        child=self.root/'nested';child.mkdir();(child/'text').write_bytes(b'xyz')
        self.assertEqual(self.measure(self.root/'shared.py')['bytes'],7)
        self.assertEqual(self.measure(self.root/'empty')['bytes'],0)
        self.assertEqual(self.measure(self.root)['bytes'],15)
        self.assertEqual(self.measure(self.root/'not-created',True),{'bytes':0,'state':'not_created'})
        with self.assertRaises(FileNotFoundError):self.measure(self.root/'not-created')
    def test_links_special_parent_and_identity_drift(self):
        path=self.root/'ordinary';path.write_bytes(b'123')
        original_stat=os.stat
        def special(name,*args,**kwargs):
            result=original_stat(name,*args,**kwargs)
            if name=='ordinary' and kwargs.get('dir_fd') is not None:
                values=list(result);values[0]=stat.S_IFLNK|0o777;return os.stat_result(values)
            return result
        with patch('os.stat',side_effect=special):
            with self.assertRaises(ValueError):self.measure(path)
            with self.assertRaises(ValueError):self.measure(path,True)
        original_fstat=os.fstat
        def drift(fd):
            result=original_fstat(fd)
            if stat.S_ISREG(result.st_mode):
                values=list(result);values[1]+=1;return os.stat_result(values)
            return result
        with patch('os.fstat',side_effect=drift):
            with self.assertRaises(ValueError):self.measure(path)
        original_open=os.open
        def denied(name,*args,**kwargs):
            if name=='ordinary':raise PermissionError('injected read failure')
            return original_open(name,*args,**kwargs)
        with patch('os.open',side_effect=denied):
            with self.assertRaises(PermissionError):self.measure(path)
        with self.assertRaises(ValueError):self.measure(self.root/'..'/'outside')
    def test_anchored_directory_scanner_identity(self):
        folder=self.root/'folder';folder.mkdir();(folder/'item').write_bytes(b'1234567')
        self.assertEqual(self.measure(folder)['bytes'],7)

class ReservationTests(unittest.TestCase):
    def setUp(self):
        temporary=tempfile.TemporaryDirectory(prefix='r019-reservation-',dir=os.environ['TMPDIR']);self.addCleanup(temporary.cleanup)
        self.repository=Path(temporary.name)/'repository';self.stage=self.repository/'.cache/v0.5.1-s4'
        (self.stage/'leader').mkdir(parents=True)
        (self.repository/'benchmark/tail-localization').mkdir(parents=True)
        self.authorization=json.loads((FORMAL/'leader/authorization.json').read_text())
        for role in ('builder','reviewer'):
            (self.stage/role/'cache').mkdir(parents=True)
            (self.stage/role/'tmp').mkdir()
            history=json.loads((FORMAL/role/'ledger.json').read_text())
            # Isolate candidate control state; retained legacy rows are copied unchanged.
            history['runs']=[row for row in history['runs'] if row['run_id']=='run-r015-build-001']
            write(self.stage/role/'ledger.json',history)
        for name in ('r016-control-debt-002.json','r015-check-admission-failure-001.json',
                     'r002-current-builder-upper-bound.json','r007-current-builder-upper-bound.json','r019-control-debt-001.json','r020-control-debt-001.json','r020-protected-source-001.json'):
            (self.stage/'leader'/name).write_bytes((FORMAL/'leader'/name).read_bytes())
        self.authorization['r016']['control_debt_path']=str(self.stage/'leader/r016-control-debt-002.json')
        self.authorization['r019']['control_debt_path']=str(self.stage/'leader/r019-control-debt-001.json')
        self.authorization['r020']['control_debt_path']=str(self.stage/'leader/r020-control-debt-001.json')
        for key,item in self.authorization['r020']['invocations'].items():
            item['called_path']=str(self.stage/'leader'/Path(item['called_path']).name)
            item['used_path']=str(self.stage/item['role']/'cache'/Path(item['used_path']).name)
        write(self.stage/'leader/authorization.json',self.authorization)
        self.saved=admission.CURRENT_CLAIM;self.addCleanup(setattr,admission,'CURRENT_CLAIM',self.saved)
    def claim(self,role,run,seconds,kind):
        source=self.repository/'benchmark/tail-localization/localize_v17.py'
        argv=['/usr/bin/python3',str(source),'--role',role,'--run-id',run,'--kind',kind,'--seconds',str(seconds),'--r019-called-sha256','R019_CALLED_SHA256','--','/usr/bin/python3','-I','fixture-only-no-execution.py']
        row={'run_id':run,'kind':kind,'seconds':seconds,'argv':argv,'environment':{}}
        table=self.stage/role/'cache'/('r019-'+run+'-commands.json');write(table,{'commands':[row]})
        seal=self.stage/role/'cache'/('r019-'+run+'-seal.json');write(seal,{'fixture':'only isolated regular data'})
        key=admission.invocation_key(role,run);expected=self.authorization['r020']['invocations'][key]
        called={'schema':'r019-invocation-called-v1','role':role,'run_id':run,'attempt':expected['attempt'],
                'command_table_path':str(table),'command_table_sha256':digest(table),
                'command_row_sha256':hashlib.sha256(json.dumps(row,sort_keys=True,separators=(',',':')).encode()).hexdigest(),
                'execution_seal_path':str(seal),'execution_seal_sha256':digest(seal),
                'control_debt_sha256':self.authorization['r019']['control_debt_sha256'],
                'r020_control_debt_sha256':self.authorization['r020']['control_debt_sha256']}
        path=Path(expected['called_path']);write(path,called);argv[argv.index('--r019-called-sha256')+1]=digest(path)
        with patch.object(wrapper,'__file__',str(source)):
            credential=wrapper.claim_before_candidate_import(argv)
        admission.CURRENT_CLAIM=credential
        return wrapper.parse_arguments(argv[2:])
    def settle(self,role,run,seconds,kind):
        args=self.claim(role,run,seconds,kind)
        self.assertIs(type(args.seconds),float)
        reservation=budget.Reservation(self.stage,role,run,kind,args.seconds)
        with reservation:
            self.assertGreater(reservation.output_bytes(),0)
            if run=='run-r018-build-001':
                (reservation.output/'build-output').mkdir()
                write(reservation.output/'build-output/build-receipt.json',{'schema':'isolated-no-build-receipt'})
            admission.original.record_build_artifact(args,reservation,{'HP_BASELINE_PACKAGE_SHA256':digest(REPOSITORY/'benchmark/tail-localization/request-boundaries/inputs-lock.json')})
        write(reservation.output/'cleanup.json',{'complete':True,'forced':False,'errors':[],'remaining':[],'unknown':[]})
        ledger=json.loads((self.stage/role/'ledger.json').read_text())
        row=[row for row in ledger['runs'] if row['run_id']==run][0]
        self.assertEqual(row['status'],'valid');self.assertEqual(row['byte_classification_status'],'verified');self.assertEqual(row['accounting_errors'],[])
        self.assertIs(type(row['reserved_seconds']),float)
        return row
    def test_real_check20_then_recovery90_normal_settlement_consumer(self):
        self.settle('builder','run-r019-check-001',20,'selfcheck')
        self.settle('reviewer','run-r019-check-001',20,'selfcheck')
        self.settle('builder','run-r018-build-001',90,'selfcheck')
        self.assertTrue(admission.check_slot(self.stage,'builder','run-r018-check-001','ctest',60.0,self.authorization))
        with self.assertRaises(FileExistsError):self.claim('builder','run-r018-build-001',90,'selfcheck')
    def test_complete_plan_and_category_limits(self):
        self.claim('builder','run-r019-check-001',20,'selfcheck')
        self.assertTrue(admission.check_slot(self.stage,'builder','run-r019-check-001','selfcheck',20.0,self.authorization))
        limited=copy.deepcopy(self.authorization);limited['per_role_dynamic_seconds']=400
        with self.assertRaises(ValueError):admission.check_slot(self.stage,'builder','run-r019-check-001','selfcheck',20.0,limited)
        self.assertGreater(admission.capacity(self.stage,'builder',self.authorization,budget.scan_bytes,budget.fixture_link_bytes),0)
        limited=copy.deepcopy(self.authorization);limited['r018']['build_output_bytes']=1
        with self.assertRaises(ValueError):admission.capacity(self.stage,'builder',limited,budget.scan_bytes,budget.fixture_link_bytes)
        limited=copy.deepcopy(self.authorization);limited['r019']['per_role_output_bytes']=1
        with self.assertRaises(ValueError):admission.capacity(self.stage,'builder',limited,budget.scan_bytes,budget.fixture_link_bytes)
        for kind,seconds in [('selfcheck',True),('selfcheck',20.5),('ctest',20.0)]:
            with self.assertRaises(ValueError):admission.check_slot(self.stage,'builder','run-r019-check-001',kind,seconds,self.authorization)
    def test_real_six_groups_and_new_prefix(self):
        quantities={'build-001':1,'check-001':2,'smoke-001':3,'decode-001':4,'boundary-01':5,'boundary-02':6,'boundary-03':7}
        for suffix,amount in quantities.items():
            root=self.stage/'builder'/('run-r018-'+suffix);root.mkdir();(root/'bytes').write_bytes(b'x'*amount)
        scanner=lambda path:measure_declared_root(path,budget.scan_bytes,budget.fixture_link_bytes)['bytes']
        source=REPOSITORY/'benchmark/tail-localization/request-boundaries'
        expected=sum(path.stat().st_size for path in source.rglob('*') if path.is_file())
        expected+=sum((REPOSITORY/'benchmark/tail-localization'/name).stat().st_size for name in ('r018_admission.py','budget_v13.py','localize_v15.py'))
        expected+=28+self.authorization['r018']['role_output_high_water_bytes']['builder']
        self.assertEqual(admission.original.capacity(self.stage,'builder',self.authorization,scanner),expected)
        before=admission.capacity(self.stage,'builder',self.authorization,budget.scan_bytes,budget.fixture_link_bytes)
        (self.stage/'builder/cache/r019-new.json').write_bytes(b'12345')
        self.assertEqual(admission.capacity(self.stage,'builder',self.authorization,budget.scan_bytes,budget.fixture_link_bytes)-before,5)
        capped=copy.deepcopy(self.authorization);capped['per_role_output_bytes']=expected
        with self.assertRaises(ValueError):admission.capacity(self.stage,'builder',capped,budget.scan_bytes,budget.fixture_link_bytes)

    def test_current_unknown_and_cross_role_failure_stop(self):
        self.settle('builder','run-r019-check-001',20,'selfcheck')
        self.settle('reviewer','run-r019-check-001',20,'selfcheck')
        admission.CURRENT_CLAIM=None
        specification=self.authorization['r020']['invocations']['builder_build']
        write(Path(specification['called_path']),{'committed':True})
        with self.assertRaisesRegex(ValueError,'called committed without used'):
            admission.check_slot(self.stage,'reviewer','run-r018-build-001','selfcheck',90.0,self.authorization)

if __name__=='__main__':unittest.main()
