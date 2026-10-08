"""Approved R002 accounting correction only; never invoke CTest or a workload."""
import hashlib
import json
from pathlib import Path
from budget import atomic_json


def reconcile():
    repository = Path(__file__).resolve().parents[2]
    role_root = repository / ".cache/v0.5.1-s4/builder"
    before = role_root / "baseline-v2-failed-seal/ledger.before-reconcile.json"
    ledger_path = role_root / "ledger.json"
    data = before.read_bytes()
    if ledger_path.read_bytes() != data:
        raise RuntimeError("ledger drift from preserved pre-reconciliation bytes")
    ledger = json.loads(data)
    targets = [run for run in ledger["runs"] if run["run_id"] == "run-baseline-ctest-001"]
    if len(targets) != 1 or targets[0]["status"] != "running" or targets[0]["reserved_seconds"] != 120:
        raise RuntimeError("unexpected target accounting")
    target = targets[0]
    target.update(status="invalid", charged_seconds=120.0, role_output_bytes=None,
                  byte_classification_status="unknown", observed_elapsed_seconds=None,
                  error="v2 guard rejected original controlled testcase symlink; final byte classification raised before settlement",
                  reconciliation=dict(authority="docs/leader/reworks/V0.5.1/S4-rework-002.md",
                    before_ledger_sha256=hashlib.sha256(data).hexdigest(),
                    method="conservative full original reservation; no guessed elapsed; no retry"))
    atomic_json(ledger_path, ledger)
    atomic_json(role_root / "run-baseline-ctest-001/ledger-reconciliation.json", target["reconciliation"])


if __name__ == "__main__":
    reconcile()
