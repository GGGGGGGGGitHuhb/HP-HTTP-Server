"""R018 bounded regular-file extraction, exclusively in the charged build slot."""
import argparse
import hashlib
import importlib.util
import io
import json
import os
import pathlib
import shutil
import stat
import subprocess
import sys
import tarfile
import time
sys.dont_write_bytecode = True
_dependency = pathlib.Path(__file__).absolute().parent / 'prepare_package.py'
for _path in [*reversed(_dependency.parents), _dependency]:
    _mode = _path.lstat().st_mode
    if stat.S_ISLNK(_mode):
        raise ValueError('linked package import path')
_spec = importlib.util.spec_from_file_location('baseline_prepare_package', _dependency)
_module = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_module)
prepare_package = _module.prepare_package
verify_package = _module.verify_package
validate_directory_path = _module.validate_directory_path

TOOLS = {'cc': '/usr/bin/cc', 'cxx': '/usr/bin/c++', 'cmake': '/usr/bin/cmake',
         'make': '/usr/bin/make', 'python': '/usr/bin/python3', 'patch': '/usr/bin/patch',
         'ldd': '/usr/bin/ldd', 'dpkg_deb': '/usr/bin/dpkg-deb',
         'assembler': '/usr/bin/as', 'linker': '/usr/bin/ld',
         'archiver': '/usr/bin/ar', 'ranlib': '/usr/bin/ranlib'}
CLIENT_SOURCES = ['wrk.c', 'net.c', 'ssl.c', 'aprintf.c', 'stats.c', 'script.c',
                  'units.c', 'ae.c', 'zmalloc.c', 'http_parser.c',
                  'MeasurementWindow.c', 'HistogramExport.c']


def extract_archive(stream, destination, deadline, regular_prefixes=None, ignored_links=None):
    destination.mkdir(parents=True, exist_ok=False)
    skipped_links = []
    with tarfile.open(fileobj=stream, mode='r:*') as archive:
        members = archive.getmembers()
        if len(members) > 20000 or sum(m.size for m in members) > 32 * 1024 * 1024:
            raise ValueError('archive capacity exceeded')
        for member in members:
            if time.monotonic() >= deadline:
                raise TimeoutError('archive verification deadline')
            name = pathlib.PurePosixPath(member.name)
            if name.is_absolute() or '..' in name.parts or not (member.isfile() or member.isdir() or member.issym()):
                raise ValueError('unsafe archive member')
            if member.issym():
                target = pathlib.PurePosixPath(member.linkname)
                if target.is_absolute():
                    raise ValueError('absolute archive link')
                resolved = (destination / name.parent / target).resolve()
                if not resolved.is_relative_to(destination.resolve()):
                    raise ValueError('escaping archive link')
                normalized = name.as_posix()
                if ignored_links is None or ignored_links.get(normalized) != member.linkname:
                    raise ValueError('undeclared archive symlink')
                skipped_links.append({'path':normalized,'target':member.linkname,'reason':'unused Debian alias; not created'})
        # Extract regular files first; never traverse a previously created link.
        for member in members:
            if time.monotonic() >= deadline:
                raise TimeoutError('archive extraction deadline')
            target = destination / member.name
            normalized = pathlib.PurePosixPath(member.name).as_posix()
            if regular_prefixes is not None and not (member.isfile() and
                    any(normalized == prefix or normalized.startswith(prefix + '/') for prefix in regular_prefixes)):
                continue
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.extractfile(member) as source, target.open('xb') as output:
                    shutil.copyfileobj(source, output)
                target.chmod(member.mode & 0o777)
    return skipped_links


def extract_deb(path, destination, environment, deadline):
    # dpkg-deb decodes the declared compression; our extractor owns path safety.
    result = subprocess.run([TOOLS['dpkg_deb'], '--fsys-tarfile', str(path)],
                            env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=max(0.1, deadline - time.monotonic()), check=True)
    if len(result.stdout) > 32 * 1024 * 1024:
        raise ValueError('Debian payload capacity exceeded')
    prefixes = ['usr/include/luajit-2.1'] if path.name == 'luajit-dev.deb' else ['usr/bin/luajit']
    ignored_links = {'usr/lib/x86_64-linux-gnu/libluajit-5.1.so':'libluajit-5.1.so.2.1.1703358377',
                     'usr/share/doc/libluajit-5.1-dev/changelog.Debian.gz':'../libluajit-5.1-2/changelog.Debian.gz'} if path.name == 'luajit-dev.deb' else {
                     'usr/share/doc/luajit/changelog.Debian.gz':'../libluajit-5.1-2/changelog.Debian.gz'}
    skipped = extract_archive(io.BytesIO(result.stdout), destination, deadline,
                              regular_prefixes=prefixes, ignored_links=ignored_links)
    return {'input':str(path),'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),
            'regularPrefixes':prefixes,'skippedLinks':skipped}


