"""R002 read-only role byte reclassification; emit only an appended receipt."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import time
import stat
from budget_v3 import Reservation, atomic_json, fixture_link_bytes


def reclassify(expected_inventory_sha256):
    repository = Path(__file__).resolve().parents[2]
    stage = repository / ".cache/v0.5.1-s4"
    role_root = stage / "builder"
    ledger_path = role_root / "ledger.json"
    ledger_bytes = ledger_path.read_bytes()
    ledger = json.loads(ledger_bytes)
    run = next(item for item in ledger["runs"] if item["run_id"] == "run-baseline-ctest-001")
    if run["status"] != "invalid" or run["charged_seconds"] != 120 or run["byte_classification_status"] != "unknown":
        raise ValueError("unexpected original failure accounting")
    reservation = Reservation(stage, "builder", "run-reclassification-metadata", "selfcheck", 20,
                              expected_inventory_sha256, "static-inventory-v3.json")
    reservation.static_inventory = json.loads((role_root / "static-inventory-v3.json").read_text())["files"]
    reservation.verify_static_hashes()
    size = reservation.output_bytes()
    upper_bound = 0
    links = []
    for directory, folders, filenames in os.walk(role_root, followlinks=False):
        for name in folders + filenames:
            path = Path(directory, name)
            status = path.lstat()
            if stat.S_ISDIR(status.st_mode):
                continue
            if not stat.S_ISREG(status.st_mode) and not stat.S_ISLNK(status.st_mode):
                raise ValueError("non-regular/non-link artifact")
            upper_bound += status.st_size
            if not path.is_symlink():
                continue
            relative = str(path.relative_to(role_root))
            expected = reservation.static_inventory.get(relative)
            if expected and expected.get("type") == "symlink" and os.readlink(path) == expected["target"]:
                classification = "sealed_static_input"
            else:
                registered = next((role_root / prefix for prefix in ("build-E/test-tmp", "build-observed/test-tmp") if path.is_relative_to(role_root / prefix)), None)
                fixture_link_bytes(path, registered)
                classification = "original_owned_fixture"
            links.append(dict(path=relative, raw_target=os.readlink(path), normalized_target=str(path.resolve()),
                              classification=classification, lstat_bytes=status.st_size))
        folders[:] = [name for name in folders if not Path(directory, name).is_symlink()]
    if ledger_path.read_bytes() != ledger_bytes:
        raise ValueError("ledger changed during read-only classification")
    receipt = dict(schema=1, run_id=run["run_id"], verified=True, role=str(role_root), role_dynamic_bytes=size,
                   role_total_bytes_upper_bound=upper_bound, links=links,
                   monotonic=time.monotonic(), original_ledger_sha256=hashlib.sha256(ledger_bytes).hexdigest(),
                   inventory_sha256=expected_inventory_sha256,
                   method="current full role tree regular files + links own lstat sizes; no directory-link traversal; static build included as conservative bound",
                   original_failure_retained=True, original_null_byte_field_retained=True)
    atomic_json(role_root / run["run_id"] / "byte-reclassification-v3.json", receipt)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--inventory-sha256", required=True)
    reclassify(parser.parse_args().inventory_sha256)
