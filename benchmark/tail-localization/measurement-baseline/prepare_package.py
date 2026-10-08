"""Verify and copy a frozen package; no historical cache lookup."""
import hashlib
import importlib.util
import json
import os
import pathlib
import shutil
import stat


def validate_directory_path(path):
    absolute = pathlib.Path(path).absolute()
    for component in [*reversed(absolute.parents), absolute]:
        mode = component.lstat().st_mode
        if not stat.S_ISDIR(mode) or stat.S_ISLNK(mode):
            raise ValueError('directory path must contain only real directories')
    return absolute


def verify_package(package_root):
    root = validate_directory_path(package_root)
    actual = set()
    for path in root.rglob('*'):
        mode = path.lstat().st_mode
        if stat.S_ISLNK(mode) or not (stat.S_ISREG(mode) or stat.S_ISDIR(mode)):
            raise ValueError('package links and special files are forbidden')
        if stat.S_ISREG(mode):
            actual.add(path.relative_to(root).as_posix())
    lock = json.loads((root / 'inputs-lock.json').read_text())
    if lock.get('schema') != 'baseline-v1-input-lock' or lock.get('serverCommit') != 'acda3f92d42a36d0b0554e185bc6f4155b4e5889':
        raise ValueError('package lock contract/source mismatch')
    declared = {item['path'] for item in lock['files']} | {'inputs-lock.json'}
    if actual != declared or len(lock['files']) != len(declared) - 1:
        raise ValueError('package file set or duplicate lock entry mismatch')
    for item in lock['files']:
        if not item.get('source') or not item.get('license') or item.get('required') is not True:
            raise ValueError('missing input provenance or license')
        if not isinstance(item.get('bytes'), int) or item['bytes'] < 0 or len(item.get('sha256', '')) != 64:
            raise ValueError('invalid input size or SHA identity')
        relative = pathlib.PurePosixPath(item['path'])
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError('unsafe lock path')
        path = root / relative
        if path.is_symlink() or not path.is_file():
            raise ValueError('missing or linked package input: ' + str(relative))
        if path.stat().st_size != item['bytes'] or hashlib.sha256(path.read_bytes()).hexdigest() != item['sha256']:
            raise ValueError('package input drift: ' + str(relative))
    return lock


def prepare_package(package_root, destination):
    root = validate_directory_path(package_root)
    target = pathlib.Path(destination).absolute()
    validate_directory_path(target.parent)
    if target.exists() or root == target or root in target.parents:
        raise ValueError('copy destination must be new and outside package')
    verify_package(root)
    shutil.copytree(root, target, symlinks=False)
    verify_package(target)
    return target


def verify_execution_context(package_root, output_root, run_id):
    root = validate_directory_path(package_root)
    verify_package(root)
    if os.environ.get('HP_BASELINE_ROLE') not in ('builder','reviewer') or os.environ.get('HP_BASELINE_RUN') != run_id:
        raise ValueError('missing exact role/run execution license')
    if os.environ.get('HP_BASELINE_OUTPUT_ROOT') != str(output_root):
        raise ValueError('execution output root mismatch')
    if hashlib.sha256((root/'inputs-lock.json').read_bytes()).hexdigest() != os.environ.get('HP_BASELINE_PACKAGE_SHA256'):
        raise ValueError('execution package identity mismatch')
    spec = importlib.util.spec_from_file_location('baseline_context_owned',root/'owned_process.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    parent = module.process_identity(os.getppid())
    if not parent or parent['pid'] != int(os.environ['HP_BASELINE_GOVERNOR_PID']) or parent['starttime'] != int(os.environ['HP_BASELINE_GOVERNOR_STARTTIME']):
        raise ValueError('execution governor identity mismatch')
