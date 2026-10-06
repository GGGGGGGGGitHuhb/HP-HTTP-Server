#!/usr/bin/env python3
import io
import pathlib
import tempfile
import unittest
from unittest.mock import patch
import tail_diagnostic as t

class TailTests(unittest.TestCase):
    def test_schedule(self):
        s=t.schedule();self.assertEqual([(x['scenario'],x['threads']) for x in s],[('P1',1),('P3',2),('P3',4),('P3',4),('P3',2),('P1',1)])
        self.assertTrue(all(x['label']=='D' and x['size']==1024 for x in s))
        self.assertTrue(all(x['workers']==4 and x['connections']==128 for x in s if x['scenario']=='P3'))
    def test_unknown(self):
        x=t.read('/proc/does-not-exist-r003');self.assertIsNone(x['value']);self.assertIsNotNone(x['error'])
    def test_stat_spaces(self):
        vals=['S']+['0']*18+['123']
        vals[11]='7';vals[12]='4'
        self.assertEqual(t.stat_identity('5 (name with spaces) '+' '.join(vals))['starttime'],123)
    def test_missing_delta(self):
        a={'pid':1,'tid':2,'starttime':3,'wait_ns':None};b=a|{'wait_ns':5}
        self.assertIsNone(t.adjacent_delta(a,b)['wait_ns'])
    def test_reused_tid(self):
        self.assertIsNone(t.adjacent_delta({'pid':1,'tid':2,'starttime':3},{'pid':1,'tid':2,'starttime':4}))
    def test_negative_delta(self):
        a={'pid':1,'tid':2,'starttime':3,'wait_ns':10}
        self.assertIsNone(t.adjacent_delta(a,a|{'wait_ns':5})['wait_ns'])
    def test_sampler_failure_latched(self):
        with tempfile.TemporaryDirectory() as d:
            o=t.Observer(pathlib.Path(d)/'timeline');o.stream.close();o.tick()
            self.assertIsNotNone(o.failure);n=o.count;o.tick();self.assertEqual(o.count,n);o.close()
    def test_alive_cleanup_unaffected_by_sampler_error(self):
        with tempfile.TemporaryDirectory() as d:
            o=t.Observer(pathlib.Path(d)/'timeline');o.failure='injected'
            klass=t.observed_process(o)
            with patch.object(t.legacy.OwnedProcess,'alive',return_value=False):
                instance=object.__new__(klass);self.assertFalse(instance.alive())
            o.close()
    def test_cleanup_all_continues_after_error(self):
        class Owned:
            def __init__(self,fail):self.fail=fail;self.called=False
            def close(self):
                self.called=True
                if self.fail:raise OSError('injected')
                return {'reaped':True,'forced':False}
        a,b=Owned(True),Owned(False)
        class O:pass
        o=O();o.enabled=True;o.processes=[(a,'a'),(b,'b')]
        rows=t.cleanup_all(o);self.assertFalse(o.enabled);self.assertTrue(a.called and b.called);self.assertFalse(rows[0]['reaped']);self.assertTrue(rows[1]['reaped'])
    def test_disabled_observer_never_samples_during_cleanup(self):
        with tempfile.TemporaryDirectory() as d:
            o=t.Observer(pathlib.Path(d)/'timeline');o.enabled=False
            with patch.object(t.time,'monotonic',side_effect=KeyboardInterrupt):o.tick()
            self.assertEqual(o.count,0);o.close()
    def test_identity_drift_latches_failure(self):
        from types import SimpleNamespace
        with tempfile.TemporaryDirectory() as d:
            o=t.Observer(pathlib.Path(d)/'timeline')
            fake=SimpleNamespace(process=SimpleNamespace(pid=99999999,poll=lambda:None),identity={'starttime':1})
            o.processes=[(fake,pathlib.Path(d)/'fake')]
            with patch.object(t,'stat_identity',return_value={'starttime':2}):o.tick()
            self.assertEqual(o.failure,'process identity drift');o.close()
    def test_terminal_close_failure_visible(self):
        with tempfile.TemporaryDirectory() as d:
            o=t.Observer(pathlib.Path(d)/'timeline');o.stream.close()
            class Bad:
                def close(self):raise OSError('disk close')
            o.stream=Bad();self.assertIn('disk close',o.close()['error'])

if __name__=='__main__':unittest.main()
