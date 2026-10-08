"""Serial bounded sanitizer compilation; no test execution or sockets."""
import hashlib
import json
from pathlib import Path
import subprocess

repository=Path(__file__).resolve().parents[2]
tools=Path(__file__).resolve().parent
role=repository/'.cache/v0.5.1-s4/builder'
build=role/'E-observed/source/.test-tmp/sanitizer-build'
if build.exists():raise SystemExit('sanitizer build already exists; no implicit retry')
build.mkdir(parents=True)
flags=['-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer']
commands=[
    ['/usr/bin/g++','-std=c++17',*flags,'-I'+str(role/'E-observed/source/include/tail_localization'),
     str(tools/'test_observer_buffer.cpp'),'-o',str(build/'server-buffer-test')],
    ['/usr/bin/gcc','-std=c99','-D_GNU_SOURCE',*flags,'-I'+str(role/'wrk-observed/source/src'),
     str(role/'wrk-observed/source/src/ClientObserver.c'),str(tools/'test_client_observer.c'),
     '-o',str(build/'client-buffer-test')]]
for command in commands:subprocess.run(command,check=True,timeout=25)
files={}
for path in [build/'server-buffer-test',build/'client-buffer-test',
             tools/'test_observer_buffer.cpp',tools/'test_client_observer.c',
             role/'wrk-observed/source/src/ClientObserver.c']:
    files[str(path)]=dict(sha256=hashlib.sha256(path.read_bytes()).hexdigest(),bytes=path.stat().st_size)
(build/'build-seal.json').write_text(json.dumps(dict(commands=commands,files=files),indent=2)+'\n')
