#!/usr/bin/env python3
"""Export and build one fixed benchmark revision, without changing checkout."""
import argparse
import hashlib
import json
import pathlib
import subprocess
import tarfile

REPO = pathlib.Path(__file__).resolve().parents[1]
COMMITS = {"A": "e6aa82b5e2a95dc24bb76ff822599518a1e0d9fd",
           "B": "89514bd99a4410a6eb35d02444fe9e73f56232a2"}
TREES = {"A": "a12650f261912c55b26690df0e8e4239ede4fcf1",
         "B": "f6912c28d25355873fbca48ede3b4d63c281a97c"}
FLAGS = ["-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=OFF",
         "-DCMAKE_CXX_COMPILER=/usr/bin/g++", "-DCMAKE_CXX_STANDARD=20",
         "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG",
         "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF",
         "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1048576), b""):
            h.update(block)
    return h.hexdigest()


def archive_bytes(label):
    tree = subprocess.check_output(["git", "rev-parse", COMMITS[label] + "^{tree}"], cwd=REPO, text=True).strip()
    if tree != TREES[label]:
        raise ValueError("fixed commit/tree mismatch")
    return subprocess.check_output(["git", "archive", "--format=tar", COMMITS[label]], cwd=REPO)


def build(label, output):
    output = pathlib.Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError("build output directory must be empty")
    source, binary_dir = output / "source", output / "build"
    archive = output / "source.tar"
    archive.write_bytes(archive_bytes(label))
    source.mkdir()
    with tarfile.open(archive) as tf:
        tf.extractall(source, filter="data")
    command = ["cmake", "-S", str(source), "-B", str(binary_dir), *FLAGS]
    with (output / "configure.log").open("w") as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    build_command = ["cmake", "--build", str(binary_dir), "--target", "hp_http_server", "-j4"]
    with (output / "build.log").open("w") as log:
        subprocess.run(build_command, stdout=log, stderr=subprocess.STDOUT, check=True)
    manifest = {"schema": 1, "label": label, "commit": COMMITS[label], "tree": TREES[label],
                "archive": str(archive), "archive_sha256": sha(archive), "source": str(source),
                "source_hashes": {str(p.relative_to(source)): sha(p) for p in sorted(source.rglob("*")) if p.is_file()},
                "binary": str(binary_dir / "hp_http_server"), "binary_sha256": sha(binary_dir / "hp_http_server"),
                "cmake_cache": str(binary_dir / "CMakeCache.txt"), "cmake_cache_sha256": sha(binary_dir / "CMakeCache.txt"),
                "compile_commands": str(binary_dir / "compile_commands.json"),
                "compile_commands_sha256": sha(binary_dir / "compile_commands.json"),
                "configure_command": command, "build_command": build_command, "flags": FLAGS,
                "compiler": subprocess.check_output(["/usr/bin/g++", "--version"], text=True).splitlines()[0],
                "cmake": subprocess.check_output(["cmake", "--version"], text=True).splitlines()[0]}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(output / "manifest.json")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--revision", choices=COMMITS, required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    build(args.revision, args.output)
