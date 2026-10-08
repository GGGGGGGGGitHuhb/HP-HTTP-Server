"""Static preparation only: seal permitted source/build roots before dynamics."""
import argparse
import hashlib
import json
import os
from pathlib import Path
from budget_v6 import atomic_json

PERMITTED_ROOTS = ("wrk-source", "wrk-observed", "client-build-inputs", "E-S3", "E-observed", "build-E", "build-observed", "m1-reviewed-v1", "baseline-v2-failed-seal", "r003-route-seal")


def seal(role_root):
    role_root = Path(role_root).resolve()
    files = {}
    for name in PERMITTED_ROOTS:
        root = role_root / name
        if not root.exists():
            continue
        for directory, folders, names in os.walk(root, followlinks=False):
            if name in ("build-E", "build-observed") and Path(directory) == root:
                folders[:] = [folder for folder in folders if folder not in ("Testing", "test-tmp")]
            if name in ("E-S3", "E-observed") and Path(directory) == root / "source":
                folders[:] = [folder for folder in folders if folder != ".test-tmp"]
            if any(Path(directory, folder).is_symlink() for folder in folders):
                raise ValueError("static symlink directory not supported")
            for filename in names:
                path = Path(directory, filename)
                status = path.lstat()
                item = dict(size=status.st_size, mtime_ns=status.st_mtime_ns)
                if path.is_symlink():
                    item.update(type="symlink", target=os.readlink(path))
                else:
                    digest = hashlib.sha256()
                    with path.open("rb") as source:
                        for block in iter(lambda: source.read(1024 * 1024), b""):
                            digest.update(block)
                    item.update(type="file", sha256=digest.hexdigest())
                files[str(path.relative_to(role_root))] = item
    output = role_root / "static-inventory-v6.json"
    if output.exists():
        raise ValueError("inventory already sealed; new version needs Leader approval")
    atomic_json(output, dict(schema=1, roots=list(PERMITTED_ROOTS), files=files))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("role_root")
    seal(parser.parse_args().role_root)
