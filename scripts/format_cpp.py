#!/usr/bin/env python3
"""Format tracked project C++ files without changing the Git index."""

import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys

VERSION = "18.1.3"
EXTENSIONS = {".cpp", ".h", ".cc", ".hpp", ".cxx", ".hxx"}
EXCLUDED = {"third_party", "vendor", "generated", "build", ".cache"}


def git(*args):
    return subprocess.check_output(["git", *args])


def names(*args):
    return {p.decode("utf-8", "surrogateescape") for p in git(*args).split(b"\0") if p}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--hook", action="store_true")
    args = parser.parse_args()
    root = pathlib.Path(git("rev-parse", "--show-toplevel").decode().strip())
    os.chdir(root)
    executable = shutil.which("clang-format-18") or shutil.which("clang-format")
    if not executable:
        raise RuntimeError(f"需要 clang-format {VERSION}，未执行格式化。")
    version = subprocess.check_output([executable, "--version"], text=True)
    match = re.search(r"version (\d+\.\d+\.\d+)", version)
    if not match or match.group(1) != VERSION:
        raise RuntimeError(f"需要 clang-format {VERSION}；当前：{version.strip()}")
    if git("ls-files", "-u"):
        raise RuntimeError("存在未解决的合并冲突，请先解决。")
    files = sorted(p for p in names("ls-files", "-z")
                   if pathlib.Path(p).suffix in EXTENSIONS
                   and not any(part in EXCLUDED or part.startswith("build-")
                               for part in pathlib.Path(p).parts))
    if args.hook:
        staged = names("diff", "--cached", "--name-only", "-z")
        dirty = names("diff", "--name-only", "-z")
        if ".clang-format" in dirty:
            raise RuntimeError("格式配置有未暂存修改，请先确认并暂存配置。")
        protected = set(files) | {".clang-format", "scripts/format_cpp.py", ".githooks/pre-commit"}
        partial = staged & dirty & protected
        if partial:
            raise RuntimeError("存在部分暂存的格式化相关文件，未修改任何文件：" + ", ".join(sorted(partial)))
    changes = []
    # Compute all results before writing, so tool errors cannot leave a partial run.
    for name in files:
        path = pathlib.Path(name)
        if not path.exists():
            continue
        if path.is_symlink():
            raise RuntimeError("拒绝格式化符号链接：" + name)
        before = path.read_bytes()
        after = subprocess.check_output(
            [executable, "--style=file", "--assume-filename=" + name], input=before)
        if before != after:
            changes.append((path, after))
    if not args.check:
        for path, content in changes:
            path.write_bytes(content)
    if changes:
        for path, _ in changes:
            print(path)
        if args.hook or args.check:
            raise RuntimeError("发现格式差异；请查看差异并按需重新暂存后提交。未自动暂存文件。")
    if args.hook:
        # The commit uses index content, which may differ from the working tree.
        for name in files:
            indexed = git("show", ":" + name)
            formatted = subprocess.check_output(
                [executable, "--style=file", "--assume-filename=" + name], input=indexed)
            if indexed != formatted:
                raise RuntimeError("暂存区仍有格式差异，请确认并暂存：" + name)
    print(f"clang-format {VERSION}: {len(files)} files; {len(changes)} changed")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"format: {error}", file=sys.stderr)
        sys.exit(1)
