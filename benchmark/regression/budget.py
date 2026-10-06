"""Fail-closed durable role ledger, reserved before any runtime subprocess."""
import fcntl
import json
import math
import os
import pathlib
import time
import uuid
from identity import legacy

TOTAL_SECONDS = 1800
SUITE_SECONDS = 900

def atomic(path, data):
    temporary = path.with_suffix('.pending')
    with temporary.open('w') as stream:
        json.dump(data, stream, indent=2, allow_nan=False)
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(path)

def new_output(root, output):
    root = pathlib.Path(root).absolute()
    output = pathlib.Path(output).absolute()
    legacy.demand(root == root.resolve() and output == output.resolve(), 'symbolic/escaped output rejected')
    legacy.demand(output.parent == root and output.name.startswith('run-') and not output.exists(), 'new direct role run-* output required')
    return output

def number(value):
    legacy.demand(type(value) in (int, float) and math.isfinite(value) and value >= 0, 'corrupt ledger duration')
    return value

def load(path):
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError) as error:
        raise legacy.Invalid('corrupt ledger/record: ' + str(path)) from error

def charged(root, data):
    legacy.demand(data.get('schema') == 1 and isinstance(data.get('entries'), dict), 'corrupt ledger schema')
    total = 0
    for key, entry in data['entries'].items():
        legacy.demand(entry.get('state') in ('running', 'finished'), 'corrupt ledger state')
        total += number(entry['reserved_seconds'] if entry['state'] == 'running' else entry['charged_seconds'])
    seen = set()
    for path in root.rglob('run.json'):
        legacy.demand(not path.is_symlink(), 'symbolic historical record')
        row = load(path)
        key = row.get('run_id')
        if key:
            legacy.demand(key not in seen, 'duplicate historical run identity')
            seen.add(key)
        if key not in data['entries']:
            # Unknown completed runs are charged recursively; unfinished runs reserve a whole suite.
            total += number(row['wall_seconds']) if row.get('ended_utc') and 'wall_seconds' in row else SUITE_SECONDS
    return total

class Reservation:
    def __init__(self, root, output, kind, allowance=SUITE_SECONDS):
        self.root = pathlib.Path(root)
        self.root.mkdir(parents=True, exist_ok=True)
        self.lock = (self.root / 'ledger.lock').open('a')
        try:
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.output = new_output(self.root, output)
            self.path = self.root / 'ledger.json'
            self.data = load(self.path) if self.path.exists() else {'schema': 1, 'entries': {}}
            self.previous = charged(self.root, self.data)
            legacy.demand(self.previous + allowance <= TOTAL_SECONDS, 'cumulative dynamic budget exhausted')
            legacy.demand(0 < allowance <= SUITE_SECONDS, 'invalid reservation')
            self.id = str(uuid.uuid4())
            self.began = time.monotonic()
            self.allowance = allowance
            self.entry = {'kind': kind, 'output': str(self.output), 'state': 'running', 'started_utc': legacy.utc(), 'reserved_seconds': allowance}
            self.data['entries'][self.id] = self.entry
            atomic(self.path, self.data)
            self.output.mkdir()
            legacy.save(self.output / 'run.json', {'run_id': self.id, 'kind': kind, 'status': 'invalid', 'started_utc': self.entry['started_utc']})
        except BaseException:
            self.lock.close()
            raise

    def guard(self):
        legacy.demand(time.monotonic()-self.began <= self.allowance, 'suite watchdog')
        return legacy.log_guard(self.root)

    def finish(self):
        elapsed = time.monotonic()-self.began
        self.entry.update(state='finished', charged_seconds=elapsed, ended_utc=legacy.utc())
        atomic(self.path, self.data)
        self.lock.close()
        return elapsed
