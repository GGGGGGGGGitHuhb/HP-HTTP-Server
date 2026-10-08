"""R012 actual full closure compile, then startup/sequence pure counterexamples."""
import argparse
import hashlib
import importlib
import json
from pathlib import Path
import time
import unittest
from m3_pure_window_v2 import admit

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--role',choices=('builder','reviewer'),required=True);parser.add_argument('--output',type=Path,required=True);parser.add_argument('--seal',type=Path,required=True);parser.add_argument('--seal-sha256',required=True)
    args=parser.parse_args();record=admit(args.role,args.output);phase='compile-closure'
    try:
        raw=args.seal.read_bytes()
        if hashlib.sha256(raw).hexdigest()!=args.seal_sha256:raise ValueError('pure dependency seal drift')
        for item in json.loads(raw):
            if time.monotonic()>=record['start_monotonic']+13:raise TimeoutError('pure original work deadline')
            path=Path(item['path']);source=path.read_bytes()
            if hashlib.sha256(source).hexdigest()!=item['sha256']:raise ValueError('pure source drift')
            if path.suffix=='.py':compile(source,str(path),'exec')
        phase='test-import';module=importlib.import_module('test_m3_entry_v2')
        phase='tests';result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromModule(module))
        if not result.wasSuccessful():raise AssertionError('R012 pure counterexamples failed')
    except BaseException as error:
        entry=phase in ('compile-closure','test-import') and isinstance(error,(SyntaxError,IndentationError,ModuleNotFoundError,FileNotFoundError))
        (args.output/'pure-failure.json').write_text(json.dumps(dict(category='entry' if entry else 'semantic',type=type(error).__name__,phase=phase,message=str(error)),indent=2)+'\n')
        raise
