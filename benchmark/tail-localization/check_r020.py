"""一次R020 check20：先编译精确闭包，随后加载真实接缝fixture。"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import unittest
sys.dont_write_bytecode = True
ROOT = Path(__file__).absolute().parent
CLOSURE = ['check_r020.py','test_r020_collector.py','test_r020_entry.py','r020_admission.py','budget_v15.py','localize_v17.py','r019_capacity.py','r019_admission.py','budget_v14.py','localize_v16.py',
           'r018_admission.py','r016/__init__.py','r016/r015_admission.py','protection.py','proc_identity_v11.py',
           'cleanup_v11.py','watchdog_v11.py']

def load(name,path,package=False):
    spec=importlib.util.spec_from_file_location(name,path,submodule_search_locations=[str(path.parent)] if package else None)
    module=importlib.util.module_from_spec(spec);sys.modules[name]=module;spec.loader.exec_module(module)
    return module

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--role',choices=('builder','reviewer'),required=True);parser.add_argument('--output-root',type=Path,required=True)
    args=parser.parse_args()
    if os.environ['HP_R020_ROLE']!=args.role or os.environ['HP_R020_OUTPUT_ROOT']!=str(args.output_root):
        raise ValueError('R020 check runtime route')
    seal_path=Path(os.environ['HP_R020_EXECUTION_SEAL']);seal_raw=seal_path.read_bytes()
    if hashlib.sha256(seal_raw).hexdigest()!=os.environ['HP_R020_EXECUTION_SEAL_SHA256']:raise ValueError('R020 execution seal drift')
    sealed={item['path']:item for item in json.loads(seal_raw)['files']}
    stage=ROOT.parents[1]/'.cache/v0.5.1-s4'
    for owner in ('builder','reviewer'):
        table=stage/owner/'cache/r020-commands-003.json';raw=table.read_bytes();item=sealed[str(table)]
        if len(raw)!=item['bytes'] or hashlib.sha256(raw).hexdigest()!=item['sha256']:raise ValueError('R020 final command table drift')
    compiled=[]
    for name in CLOSURE:
        source=ROOT/name;raw=source.read_bytes();compile(raw,str(source),'exec');compiled.append({'path':str(source),'sha256':hashlib.sha256(raw).hexdigest()})
    load('r016',ROOT/'r016/__init__.py',True)
    for name in ('r016.r015_admission','protection','proc_identity_v11','cleanup_v11','r018_admission','r019_capacity','r019_admission','budget_v14','r020_admission','budget_v15','localize_v17'):
        load(name,ROOT/(name.replace('.','/')+'.py' if name.startswith('r016.') else name+'.py'))
    suite=unittest.TestSuite()
    for name in ('test_r020_collector','test_r020_entry'):
        fixture=load(name,ROOT/(name+'.py'));suite.addTests(unittest.defaultTestLoader.loadTestsFromModule(fixture))
    result=unittest.TextTestRunner(verbosity=2).run(suite)
    receipt={'schema':'r020-check-receipt-v1','role':args.role,'status':'valid' if result.wasSuccessful() else 'invalid','tests':result.testsRun,'failures':len(result.failures),'errors':len(result.errors),'compiled':compiled,'collector_mocked':False,'collector_tests':7,'entry_tests':3,'entry_evidence':sys.modules['test_r020_entry'].RESULTS}
    (args.output_root/'check-receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    if not result.wasSuccessful():raise SystemExit(1)

if __name__=='__main__':main()
