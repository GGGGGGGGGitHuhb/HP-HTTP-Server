"""Immutable Git exports and stable execution identities; old helpers are read-only."""
import pathlib
import subprocess
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO / 'benchmark'))
import build as legacy_build
import run as legacy

COMMITS = {'C': '942f72cd9cea58e097025c3b9dd660f4132a1ffb',
           'D': '69424e6ab057bba2950c018e34c5695a4dc74f22'}
TREES = {'C': 'b407f052c7a974ae4fff4976c8275fb5905cf036',
         'D': '826e20166caa334c95a1c6fdc957a128d5aee568'}

def configure(ref=None):
    commits, trees = dict(COMMITS), dict(TREES)
    if ref:
        commits['D'] = subprocess.check_output(['git', 'rev-parse', '--verify', ref + '^{commit}'], cwd=REPO, text=True).strip()
        trees['D'] = subprocess.check_output(['git', 'rev-parse', commits['D'] + '^{tree}'], cwd=REPO, text=True).strip()
    legacy_build.COMMITS.update(commits)
    legacy_build.TREES.update(trees)
    return commits

def role_root(role):
    return REPO / '.cache' / 'v0.5.1-s3' / role

def stable_tool(data):
    return {key: data[key] for key in ('sha256', 'version', 'version_exit', 'libraries')}

def scripts():
    paths = list(HERE.glob('*.py')) + [REPO / 'benchmark' / name for name in ('run.py', 'build.py', 'summary.lua')]
    return {str(p.relative_to(REPO)): legacy.sha(p) for p in sorted(paths)}

def snapshot(root, wrk):
    manifests = {label: legacy.validate_manifest(root / label / 'manifest.json', label) for label in ('C', 'D')}
    for label, manifest in manifests.items():
        legacy.demand(pathlib.Path(manifest['source']) == root / label / 'source' and pathlib.Path(manifest['binary']) == root / label / 'build/hp_http_server', 'manifest not owned by this role')
    dependencies = {}
    for label, manifest in manifests.items():
        listing = subprocess.check_output(['ldd', manifest['binary']], text=True)
        legacy.demand('not found' not in listing, 'server dependency missing')
        dependencies[label] = {match.group(1): legacy.sha(match.group(1)) for line in listing.splitlines() if (match := re.search(r'(?:=>\s+)?(/\S+)\s+\(', line))}
    compiler = {'path': str(pathlib.Path('/usr/bin/g++').resolve()), 'sha256': legacy.sha('/usr/bin/g++')}
    return {'manifests': manifests, 'wrk': stable_tool(legacy.validate_tool(wrk)), 'scripts': scripts(), 'compiler': compiler, 'server_libraries': dependencies}

configure()
