"""一次R019 check20：先编译精确闭包，随后加载真实接缝fixture。"""
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
CLOSURE = ['check_r019.py','test_r019.py','r019_capacity.py','r019_admission.py','budget_v14.py','localize_v16.py',
           'r018_admission.py','r016/__init__.py','r016/r015_admission.py','protection.py','proc_identity_v11.py',
           'cleanup_v11.py','watchdog_v11.py']

def load(name,path,package=False):
    spec=importlib.util.spec_from_file_location(name,path,submodule_search_locations=[str(path.parent)] if package else None)
    module=importlib.util.module_from_spec(spec);sys.modules[name]=module;spec.loader.exec_module(module)
    return module

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--role',choices=('builder','reviewer'),required=True);parser.add_argument('--output-root',type=Path,required=True)
    args=parser.parse_args()
    if os.environ['HP_R019_ROLE']!=args.role or os.environ['HP_R019_OUTPUT_ROOT']!=str(args.output_root):
        raise ValueError('R019 check runtime route')
    compiled=[]
    for name in CLOSURE:
        source=ROOT/name;raw=source.read_bytes();compile(raw,str(source),'exec');compiled.append({'path':str(source),'sha256':hashlib.sha256(raw).hexdigest()})
    load('r016',ROOT/'r016/__init__.py',True)
    for name in ('r016.r015_admission','protection','proc_identity_v11','cleanup_v11','r018_admission','r019_capacity','r019_admission','budget_v14','localize_v16'):
        load(name,ROOT/(name.replace('.','/')+'.py' if name.startswith('r016.') else name+'.py'))
    fixture=load('test_r019',ROOT/'test_r019.py')
    result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromModule(fixture))
    receipt={'schema':'r019-check-receipt-v1','role':args.role,'status':'valid' if result.wasSuccessful() else 'invalid','tests':result.testsRun,'failures':len(result.failures),'errors':len(result.errors),'compiled':compiled,'collector_mocked':False}
    (args.output_root/'check-receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    if not result.wasSuccessful():raise SystemExit(1)

if __name__=='__main__':main()
