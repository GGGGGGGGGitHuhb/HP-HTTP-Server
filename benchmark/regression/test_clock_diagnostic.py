import unittest
import io
from unittest import mock
import clock_diagnostic as module
from clock_diagnostic import assess
class ClockTests(unittest.TestCase):
    def setUp(self): self.first=dict(realtime=100.,monotonic=10.,boottime=10.)
    def test_normal(self): self.assertEqual(assess(self.first,self.first,dict(realtime=100.1,monotonic=10.1,boottime=10.1))[1],[])
    def test_forward(self):
        elapsed,flags=assess(self.first,self.first,dict(realtime=134.,monotonic=10.1,boottime=10.1))
        self.assertEqual(elapsed,34); self.assertIn('clock disagreement',flags)
    def test_backward(self): self.assertIn('realtime: backward',assess(self.first,self.first,dict(realtime=99.,monotonic=10.1,boottime=10.1))[1])
    def test_pause(self):
        elapsed,flags=assess(self.first,self.first,dict(realtime=110.,monotonic=20.,boottime=20.))
        self.assertEqual(elapsed,10); self.assertTrue(any('pause' in f for f in flags))
    def test_missing(self): self.assertTrue(any('boottime' in f for f in assess(dict(realtime=1,monotonic=1),dict(realtime=1,monotonic=1),dict(realtime=1.1,monotonic=1.1))[1]))
    def test_nan(self): self.assertTrue(any('invalid' in f for f in assess(self.first,self.first,dict(realtime=float('nan'),monotonic=10.1,boottime=10.1))[1]))

class FailureTests(unittest.TestCase):
    def test_terminal_flags_invalidate_previously_observed(self):
        for flags in (['realtime: backward'],['boottime: unavailable/invalid'],['clock disagreement'],['monotonic: sampling pause/gap']):
            row={'status':'observed','kind':'diagnostic-clock','performance_acceptance':'NOT_APPLICABLE_DIAGNOSTIC'}
            module.terminal_status(row,flags)
            self.assertEqual(row['status'],'invalid')
            self.assertEqual(row['performance_acceptance'],'NOT_APPLICABLE_DIAGNOSTIC')
    def test_terminal_empty_flags_preserve_observed_or_invalid(self):
        for status in ('observed','invalid'):
            row={'status':status}
            module.terminal_status(row,[])
            self.assertEqual(row['status'],status)

    def test_unusable_available_clock(self):
        with mock.patch.object(module.time,'clock_gettime',side_effect=OSError('unusable')):
            row=module.clocks()
        self.assertTrue(all(row[k] is None for k in module.CLOCKS))
    def test_cleanup_sampling_failure_still_kills_and_waits(self):
        child=mock.Mock()
        child.process.pid=123
        child.identity={'starttime':42}
        child.alive.side_effect=[True,True,True]
        child.matches.return_value=True
        child.process.returncode=-9
        observer=mock.Mock()
        observer.tick.side_effect=OSError('timeline failed')
        with mock.patch.object(module.legacy,'log_guard',side_effect=RuntimeError('log overrun')) as guard:
            result=module.cleanup(child,observer,None,'test')
        child.process.terminate.assert_called_once()
        child.process.kill.assert_called_once()
        child.process.wait.assert_called_once_with(timeout=1)
        guard.assert_not_called()
        self.assertTrue(result['forced'] and result['reaped'])
        self.assertIn('timeline failed',result['errors'])
    def test_cleanup_clock_failure_still_reaps(self):
        child=mock.Mock()
        child.identity={'starttime':42}
        child.process.pid=123
        child.alive.side_effect=[True,True,True]
        child.matches.return_value=True
        child.process.returncode=-9
        with mock.patch.object(module,'assess',side_effect=module.legacy.Invalid('no usable clock')):
            result=module.cleanup(child,None,None,'test')
        self.assertTrue(result['forced'] and result['reaped'])
        child.process.kill.assert_called_once()
        child.process.wait.assert_called_once()
    def test_observer_init_failure_finishes_reservation(self):
        reservation=mock.Mock()
        reservation.output=__import__('pathlib').Path('/never-open')
        reservation.entry={};reservation.data={};reservation.id='test';reservation.previous=0;reservation.allowance=240
        args=mock.Mock(role='builder')
        with mock.patch.object(module.budget,'load',return_value={'entries':{}}), mock.patch.object(module.supervision,'resources'), mock.patch.object(module.budget,'Reservation',return_value=reservation), mock.patch.object(module,'Observer',side_effect=OSError('init failure')), mock.patch.object(module.budget,'atomic'), mock.patch.object(module.legacy,'save'), mock.patch.object(module.legacy,'log_bytes',return_value=0):
            code=module.diagnose(args)
        self.assertEqual(code,1)
        self.assertEqual(reservation.entry['state'],'finished')
        self.assertEqual(reservation.entry['charged_seconds'],240)
        reservation.lock.close.assert_called_once()
if __name__=='__main__': unittest.main()
