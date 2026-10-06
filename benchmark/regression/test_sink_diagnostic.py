#!/usr/bin/env python3
import pathlib
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace
import sink_diagnostic as s

class Tests(unittest.TestCase):
    def test_schedule(self):
        rows=s.schedule();self.assertEqual([r['stderr_sink'] for r in rows],['file','null','null','file'])
        for r in rows:self.assertEqual((r['label'],r['workers'],r['threads'],r['connections'],r['size']),('D',4,2,128,1024))
    def test_reject_sink(self):
        with self.assertRaises(Exception):s.sink_process(None,'other')
    def test_null_metadata_and_cleanup_on_failure(self):
        for fail in (False,True):
            with tempfile.TemporaryDirectory() as directory:
                o=SimpleNamespace(processes=[],tick=lambda:None)
                real_stat=s.os.stat
                proc=SimpleNamespace(pid=123,kill=lambda:None,wait=lambda timeout:None)
                with patch.object(s.subprocess,'Popen',return_value=proc),patch.object(s.legacy,'process_info',return_value={'pid':123,'starttime':7}),patch.object(s.os,'readlink',return_value='/bad' if fail else '/dev/null'),patch.object(s.os,'stat',side_effect=lambda p,*a,**k: real_stat('/dev/null' if str(p)=='/proc/123/fd/2' else p,*a,**k)):
                    klass=s.sink_process(o,'null')
                    if fail:
                        with self.assertRaises(Exception):klass(['server'],pathlib.Path(directory)/'server')
                        self.assertEqual(o.processes,[])
                    else:
                        obj=klass(['server'],pathlib.Path(directory)/'server')
                        self.assertEqual(obj.stderr_path,pathlib.Path('/dev/null'))
                        self.assertFalse((pathlib.Path(directory)/'server.stderr').exists())
                        self.assertEqual(len(o.processes),1);obj.stdout.close();obj.stderr.close()
    def test_wrk_stderr_remains_file(self):
        o=SimpleNamespace(processes=[],tick=lambda:None)
        with patch.object(s.legacy.OwnedProcess,'__init__',return_value=None) as init:
            s.sink_process(o,'null')(['wrk'],pathlib.Path('measurement'))
            init.assert_called_once()
    def test_file_uses_original_constructor(self):
        o=SimpleNamespace(processes=[],tick=lambda:None)
        with patch.object(s.legacy.OwnedProcess,'__init__',return_value=None) as init:
            s.sink_process(o,'file')(['server'],pathlib.Path('server'))
            init.assert_called_once()

if __name__=='__main__':unittest.main()
