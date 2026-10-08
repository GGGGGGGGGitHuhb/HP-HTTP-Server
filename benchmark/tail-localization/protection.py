"""Compare only the Leader's explicit protected files and frozen old ledgers."""
import hashlib
import json
from pathlib import Path


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def verify_protected(repository):
    repository = Path(repository).resolve()
    stage = repository / ".cache/v0.5.1-s4"
    baseline = json.loads((stage / "leader/implementation-protected-before.json").read_text())
    authorization = json.loads((stage / "leader/authorization.json").read_text())
    mismatches = []
    for relative, expected in baseline.items():
        path = repository / relative
        if ".." in Path(relative).parts or not path.resolve().is_relative_to(repository):
            raise ValueError("unsafe protection path")
        if not path.is_file() or digest(path) != expected:
            mismatches.append(relative)
    old_ledgers = {}
    for role, item in authorization["old_s3_ledgers"].items():
        path = repository / ".cache/v0.5.1-s3" / role / "ledger.json"
        matched = digest(path) == item["sha256"]
        old_ledgers[role] = matched
        if not matched:
            mismatches.append("old_s3_ledger:" + role)
    return dict(schema=1, protected_count=len(baseline), old_ledgers=old_ledgers,
                match=not mismatches, mismatches=mismatches)
