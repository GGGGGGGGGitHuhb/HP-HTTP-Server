"""R005 adapter probes: labels, original gates, freeze, supplement and clock safety."""
import copy
import json
import pathlib
import tempfile
import unittest
from unittest import mock

import batch_candidate as candidate
import budget
import identity
import model
import test_regression


def valid_rows():
    rows = [test_regression.sample(item) for item in model.schedule()]
    for row in rows:
        if row['label'] == 'D':
            row['label'] = 'E'
            row['schedule']['label'] = 'E'
    return rows


class CandidateTests(unittest.TestCase):
    def test_schedule_labels_and_smoke(self):
        schedule = candidate.schedule()
        self.assertEqual(len(schedule), 30)
        self.assertEqual(sum(item['label'] == 'E' for item in schedule), 15)
        self.assertEqual(candidate.schedule(True)[0]['label'], 'E')
        restored = copy.deepcopy(schedule)
        for item in restored:
            if item['label'] == 'E':
                item['label'] = 'D'
        self.assertEqual(restored, model.schedule())

    def test_original_gates_retained(self):
        summary = candidate.aggregate(valid_rows())
        self.assertEqual(summary['failures'], [])
        self.assertIn('P3E', summary['groups'])
        self.assertNotIn('P3D', summary['groups'])
        self.assertEqual(summary['comparison']['P3']['p99_max'], .25)
        rows = valid_rows()
        for row in rows:
            if row['label'] == 'E' and row['schedule']['scenario'] == 'P3':
                row['measurement']['latency_ms']['p99'] = 26
                test_regression.sync(row)
        self.assertIn('P3: numerical gate', candidate.aggregate(rows)['failures'])

    def test_missing_duplicate_old_candidate_rejected(self):
        for change in ('missing', 'duplicate', 'old_label'):
            rows = valid_rows()
            if change == 'missing':
                rows.pop()
            elif change == 'duplicate':
                rows[1] = copy.deepcopy(rows[0])
            else:
                rows[1]['schedule']['label'] = 'D'
            with self.assertRaises(identity.legacy.Invalid):
                candidate.aggregate(rows)

    def test_noisy_retained(self):
        rows = valid_rows()
        row = next(row for row in rows if row['label'] == 'E' and row['schedule']['scenario'] == 'P1')
        row['measurement']['qps'] *= 2
        test_regression.sync(row)
        self.assertIn('P1E: noisy', candidate.aggregate(rows)['failures'])

    def test_clock_max_and_disagreement(self):
        with mock.patch.object(candidate, 'clocks', return_value=[12, 11, 12]):
            self.assertEqual(candidate.elapsed([10, 10, 10]), 2)
        for now in ([9, 11, 11], [20, 11, 11], [float('nan'), 11, 11]):
            with mock.patch.object(candidate, 'clocks', return_value=now):
                with self.assertRaises(identity.legacy.Invalid):
                    candidate.elapsed([10, 10, 10])

    def test_approved_limits_cannot_increase(self):
        auth = {'schema': 1, 'approved_date': '2026-10-05', 'additional_seconds_per_role': 9999,
                'additional_log_bytes_per_role': 2*1024**3}
        with mock.patch.object(candidate, 'read_json', return_value=auth):
            with self.assertRaises(identity.legacy.Invalid):
                candidate.authorization('builder')

    def test_log_increment_and_history_removal(self):
        supplement = object.__new__(candidate.Supplement)
        supplement.root = pathlib.Path('/tmp')
        supplement.first = [10, 10, 10]
        supplement.reservation = mock.Mock(allowance=10)
        supplement.baseline = {'log_bytes': 100}
        supplement.auth = {'additional_log_bytes_per_role': 200}
        with mock.patch.object(candidate, 'elapsed', return_value=1):
            for size in (99, 301):
                with mock.patch.object(identity.legacy, 'log_bytes', return_value=size):
                    with self.assertRaises(identity.legacy.Invalid):
                        supplement.guard()
            with mock.patch.object(identity.legacy, 'log_bytes', return_value=300):
                self.assertEqual(supplement.guard(), 300)

    def test_supplement_charges_old_entries(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            data = {'schema': 1, 'entries': {'old': {'state': 'finished', 'charged_seconds': 1700}}}
            (root/'ledger.json').write_text(json.dumps(data))
            with mock.patch.object(budget, 'TOTAL_SECONDS', 3500):
                reservation = budget.Reservation(root, root/'run-new', 'r005-check', 180)
                self.assertEqual(reservation.previous, 1700)
                reservation.finish()
            self.assertIn('old', json.loads((root/'ledger.json').read_text())['entries'])


if __name__ == '__main__':
    unittest.main()
