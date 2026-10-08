"""Export the approved immutable E-S3 commit, never the dirty working tree."""
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile

COMMIT = "acda3f92d42a36d0b0554e185bc6f4155b4e5889"
CANDIDATE_FILES = ("CMakeLists.txt", "include/base/AsyncLogger.h", "src/base/AsyncLogger.cpp", "tests/AsyncLoggerBatch_test.cpp")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def export_candidate(repository, role_root):
    repository = Path(repository).resolve()
    role_root = Path(role_root).resolve()
    stage_root = repository / ".cache/v0.5.1-s4"
    if role_root not in (stage_root / "builder", stage_root / "reviewer"):
        raise ValueError("export must use declared role root")
    authority = json.loads((stage_root / "leader/authorization.json").read_text())
    if authority["source_commit"] != COMMIT:
        raise ValueError("fixed source commit mismatch")
    candidate = json.loads((repository / ".cache/v0.5.1-s3/leader/r005/candidate.json").read_text())
    archive = role_root / "E-S3/source.tar"
    source = archive.parent / "source"
    archive.parent.mkdir(parents=True, exist_ok=False)
    with archive.open("xb") as output:
        subprocess.run(["git", "archive", "--format=tar", COMMIT], cwd=repository,
                       stdout=output, check=True, timeout=30)
    source.mkdir()
    with tarfile.open(archive) as bundle:
        members = bundle.getmembers()
        for member in members:
            target = source / member.name
            if member.issym() or member.islnk() or not target.resolve().is_relative_to(source):
                raise ValueError("unsafe archive member")
        bundle.extractall(source, members=members, filter="data")
    hashes = {str(path.relative_to(source)): sha256(path) for path in sorted(source.rglob("*")) if path.is_file()}
    for name in CANDIDATE_FILES:
        if hashes[name] != candidate["changed_hashes"][name]:
            raise ValueError("historical E source differs: " + name)
    manifest = dict(schema=1, label="E-S3", source_commit=COMMIT, source=str(source),
                    archive_sha256=sha256(archive), source_hashes=hashes,
                    historical_patch_sha256=candidate["patch_sha256"],
                    limits=["new commit archive identity differs from historical patch archive", "production files unchanged; observer patches applied to a separate copy"])
    with (archive.parent / "manifest.json").open("x") as output:
        json.dump(manifest, output, indent=2)
        output.write("\n")
    return manifest
