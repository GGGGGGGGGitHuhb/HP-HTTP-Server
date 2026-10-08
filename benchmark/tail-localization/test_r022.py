"""R025 contract child: real sparse leaves retained for outer settlement."""
import importlib.util
import json
import os
from pathlib import Path
import sys
import unittest
import stat
from types import SimpleNamespace
sys.dont_write_bytecode=True
ROOT=Path(os.environ['HP_R022_PRODUCTION_ROOT']).absolute()

def load(name):
    path=ROOT/(name.replace('.','/')+'.py')
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec);sys.modules[name]=module;spec.loader.exec_module(module)
    return module

def main():
    package=importlib.util.spec_from_file_location('r016',ROOT/'r016/__init__.py',submodule_search_locations=[str(ROOT/'r016')])
    module=importlib.util.module_from_spec(package);sys.modules['r016']=module;package.loader.exec_module(module)
    for name in ('r016.r015_admission','protection','proc_identity_v11','cleanup_v11','r018_admission','r019_capacity','r019_admission','r020_admission','r021_admission','r022_admission','budget_v17'):
        load(name)
    admission=sys.modules['r022_admission'];budget=sys.modules['budget_v17']
    context=admission.set_contract_claim(os.environ['HP_R022_CALLED_PATH'],os.environ['HP_R022_CALLED_SHA256'])
    stage=ROOT.parents[1]/'.cache/v0.5.1-s4';role=context['called']['role']
    root=stage/role/'run-r018-build-001/build-output/source-B/.cache/r022-benchmark-tests/run-r022-check-001'
    retained=[]
    class Contract(unittest.TestCase):
        def test_real_three_sparse_rules(self):
            source=stage/role/'run-r018-build-001/build-output/source-B/benchmark'
            saved=sys.modules.get('build')
            try:
                for name in ('build','run'):
                    spec=importlib.util.spec_from_file_location(name,source/(name+'.py'))
                    module=importlib.util.module_from_spec(spec);sys.modules[name]=module;spec.loader.exec_module(module)
                runner=sys.modules['run']
            finally:
                if saved is None:sys.modules.pop('build',None)
                else:sys.modules['build']=saved
            for index,(leaf,size) in enumerate((('oversize.stdout',2147483649),('sample-01/server.stderr',1073741824),('sample-02/measurement.stdout',1073741823),('sample-02/measurement.stdout',1073741824),('sample-02/measurement.stdout',1073741825))):
                directory=root/('synthetic-r025-contract-'+str(index));directory.mkdir()
                path=directory/leaf;path.parent.mkdir(parents=True,exist_ok=True)
                with path.open('xb') as stream:stream.truncate(size)
                charged=budget.file_bytes(directory)
                self.assertEqual(charged,max(path.stat().st_blocks*512,4096))
                self.assertEqual(path.stat().st_size,size)
                if size==2147483649:
                    with self.assertRaisesRegex(Exception,'log limit exceeded'):runner.log_guard(directory)
                original=path.stat();fd=os.open(path.parent,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW)
                try:
                    fields={name:getattr(original,name) for name in ('st_dev','st_ino','st_mode','st_nlink','st_size','st_blocks')}
                    for key,value in (('st_size',17),('st_blocks',129),('st_nlink',2),('st_mode',stat.S_IFLNK|0o777)):
                        altered=SimpleNamespace(**dict(fields,**{key:value}))
                        self.assertIsNone(admission.sparse_charge(path,altered,fd,path.name))
                    self.assertIsNone(admission.sparse_charge(root/'wrong-root'/path.name,original,fd,path.name))
                finally:os.close(fd)
                retained.append(str(path))
        def test_ordinary_log_still_logical(self):
            path=root/'ordinary-contract.txt';path.write_bytes(b'abc')
            fd=os.open(root,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW)
            try:self.assertIsNone(admission.sparse_charge(path,path.stat(),fd,path.name))
            finally:os.close(fd)
        def test_stage_rejects_formal_and_old(self):
            authority=json.loads((stage/'leader/authorization.json').read_text())
            for run,kind,seconds in (('run-r018-boundary-01-O','boundary',45),('run-r018-decode-001','offline',40),('run-r018-check-001','ctest',60),('run-r018-build-001','selfcheck',90)):
                with self.assertRaisesRegex(ValueError,'five-slot'):
                    admission.check_slot(stage,'builder',run,kind,float(seconds),authority)
        def test_real_recovery_consumer(self):
            authority=json.loads((stage/'leader/authorization.json').read_text())
            bound=admission.recovery(stage,authority)
            self.assertEqual(bound['old_builder_check_row']['status'],'invalid')
            self.assertEqual(bound['old_actual_charge_seconds'],5.923150725007872)
            self.assertEqual(admission.original.valid_row(stage,role,'run-r018-build-001')['status'],'valid')
        def test_catalog_contract(self):
            overlay=load('ctest_overlay_r022')
            old=json.loads(Path(os.environ['HP_R022_ORIGINAL_CATALOG']).read_text())
            new=json.loads(Path(os.environ['HP_R022_OVERLAY_CATALOG']).read_text())
            self.assertEqual(len(overlay.compare(old,new,stage/role/'run-r018-build-001/build-output',root)),9)
    result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(Contract))
    output=Path(os.environ['HP_BASELINE_OUTPUT_ROOT'])
    (output/'contract-receipt.json').write_text(json.dumps({'schema':'r025-contract-v1','status':'valid' if result.wasSuccessful() else 'invalid','tests':result.testsRun,'failures':len(result.failures),'errors':len(result.errors),'classifier_only':True,'outer_invocation_pid':context['outer_pid'],'retained_for_outer_settlement':retained,'sparse_evidence':admission.SPARSE_EVIDENCE},indent=2)+'\n')
    if not result.wasSuccessful():raise SystemExit(1)

if __name__=='__main__':main()
