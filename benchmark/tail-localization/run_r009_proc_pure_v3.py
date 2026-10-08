"""Static syntax of every sealed Python input precedes importing corrected tests."""
import argparse
import hashlib
import importlib
import json
from pathlib import Path
import unittest

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--seal',type=Path,required=True);parser.add_argument('--seal-sha256',required=True)
    args=parser.parse_args();raw=args.seal.read_bytes()
    if hashlib.sha256(raw).hexdigest()!=args.seal_sha256:raise ValueError('pure dependency seal drift')
    for item in json.loads(raw):
        path=Path(item['path']);source=path.read_bytes()
        if hashlib.sha256(source).hexdigest()!=item['sha256']:raise ValueError('pure source drift: '+str(path))
        if path.suffix=='.py':compile(source,str(path),'exec')
    test=importlib.import_module('test_r009_proc_v3')
    result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromModule(test))
    raise SystemExit(0 if result.wasSuccessful() else 1)
