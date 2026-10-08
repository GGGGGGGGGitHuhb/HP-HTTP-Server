"""Execute two admitted no-socket buffer units; association uses the passed binary fixtures."""
import os
import hashlib
from pathlib import Path
import subprocess
import sys

role=Path(__file__).resolve().parents[2]/'.cache/v0.5.1-s4/builder'
output=Path(sys.argv[1])
if output!=role/'run-observer-sanitizer-001' or not output.is_dir() or output.is_symlink():
    raise SystemExit('not the admitted sanitizer output')
build=role/'E-observed/source/.test-tmp/sanitizer-build'
expected={'server-buffer-test':'2886b76be90e5d82679e1bbe1cc5c782395c9b1dd6f94fe769d7c8bd27fea156',
          'client-buffer-test':'b400d15656722b27baa7eae1346e7b2991f81e5abbafd6637375fa06e83b5606'}
for name,digest in expected.items():
    if hashlib.sha256((build/name).read_bytes()).hexdigest()!=digest:raise SystemExit('sanitizer binary drift')
environment=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
subprocess.run([str(build/'server-buffer-test')],env=environment,check=True,timeout=5)
subprocess.run([str(build/'client-buffer-test'),str(output)],env=environment,check=True,timeout=5)
