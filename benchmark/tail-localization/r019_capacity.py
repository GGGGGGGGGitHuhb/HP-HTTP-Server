"""R019声明根对象计量；父级FD固定，沿旧scan_bytes递归分类。"""
import os
from pathlib import Path
import stat
import sys


def identity(status):
    return status.st_dev, status.st_ino, stat.S_IFMT(status.st_mode), status.st_size


def measure_declared_root(path, scanner, fixture_link, fixture_roots=(), future=False):
    path = Path(path)
    if not path.is_absolute() or '..' in path.parts:
        raise ValueError('R019 absolute normalized root required')
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
    descriptors = []
    try:
        parent = os.open('/', flags)
        descriptors.append(parent)
        for part in path.parts[1:-1]:
            before = os.stat(part, dir_fd=parent, follow_symlinks=False)
            child = os.open(part, flags, dir_fd=parent)
            descriptors.append(child)
            if identity(before) != identity(os.fstat(child)):
                raise ValueError('R019 parent identity exchanged')
            parent = child
        name = path.name
        try:
            before = os.stat(name, dir_fd=parent, follow_symlinks=False)
        except FileNotFoundError:
            if future:
                return {'bytes': 0, 'state': 'not_created'}
            raise
        if not (stat.S_ISREG(before.st_mode) or stat.S_ISDIR(before.st_mode)):
            raise ValueError('R019 root link/unsupported object')
        opened = os.open(name, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW | os.O_CLOEXEC |
                         (os.O_DIRECTORY if stat.S_ISDIR(before.st_mode) else 0), dir_fd=parent)
        descriptors.append(opened)
        if identity(before) != identity(os.fstat(opened)):
            raise ValueError('R019 root identity exchanged')
        if stat.S_ISREG(before.st_mode):
            size = before.st_size
        else:
            anchored = Path('/proc/self/fd') / str(opened) / '.'
            # Keep the trailing /. literal: Path normalizes it away otherwise.
            anchored = str(anchored) + '/.'
            def classify(relative, actual, status, target):
                original = path / relative
                if target is None:
                    return status.st_size
                boundary = next((root for root in fixture_roots if original.is_relative_to(root)), None)
                return fixture_link(original, boundary, status, target)
            size = scanner(path, classify, lambda relative: bool(relative), root_open_path=anchored)
        after = os.stat(name, dir_fd=parent, follow_symlinks=False)
        signature = identity if stat.S_ISREG(before.st_mode) else lambda value: identity(value)[:3]
        if signature(before) != signature(after) or signature(before) != signature(os.fstat(opened)):
            raise ValueError('R019 root changed during measurement')
        # Verify each named ancestor still resolves to the held inode; never follow a link.
        for index, descriptor in enumerate(descriptors[:-1]):
            ancestor = Path(*path.parts[:index + 1])
            current = ancestor.lstat()
            if (current.st_dev, current.st_ino, stat.S_IFMT(current.st_mode)) != \
                    (os.fstat(descriptor).st_dev, os.fstat(descriptor).st_ino, stat.S_IFMT(os.fstat(descriptor).st_mode)):
                raise ValueError('R019 ancestor changed during measurement')
        return {'bytes': size, 'state': 'present'}
    finally:
        primary = sys.exc_info()[1]
        close_errors = []
        for descriptor in reversed(descriptors):
            try:
                os.close(descriptor)
            except OSError as error:
                close_errors.append(error)
        if close_errors:
            if primary is not None:
                primary.add_note('R019 descriptor close failures: ' + repr(close_errors))
            else:
                raise close_errors[0]
