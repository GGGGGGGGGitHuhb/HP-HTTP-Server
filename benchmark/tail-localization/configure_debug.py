"""R004 fixed Debug route: configure original source, adjust generated ENV only."""
from pathlib import Path
import subprocess
import json
import hashlib

role = Path(__file__).resolve().parents[2] / '.cache/v0.5.1-s4/builder'
source = role / 'E-S3/source'
build = source / '.test-tmp/debug-build'
if build.exists():
    raise SystemExit('Debug build already exists; no implicit retry')
result = subprocess.run(['/usr/bin/cmake', '-S', str(source), '-B', str(build),
                         '-DCMAKE_BUILD_TYPE=Debug', '-DBUILD_TESTING=ON'], check=False)
if result.returncode:
    raise SystemExit(result.returncode)
config = build / 'CTestTestfile.cmake'
before = config.read_bytes()
old = str(build / 'test-tmp').encode()
new = str(source / '.test-tmp').encode()
if old not in before:
    raise SystemExit('generated TMP seam missing')
after = before.replace(old, new)
(build / 'CTestTestfile.before-route.cmake').write_bytes(before)
config.write_bytes(after)
(build / 'route-identity.json').write_text(json.dumps(dict(
    source=str(source), build=str(build), build_type='Debug',
    before_sha256=hashlib.sha256(before).hexdigest(),
    after_sha256=hashlib.sha256(after).hexdigest(), replacement=[old.decode(),new.decode()]), indent=2)+'\n')
