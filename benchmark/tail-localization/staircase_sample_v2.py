from m3_contract import admit
from m3_staircase_owned import adapt_executor
from inner_identity_v11 import cleanup_step,cleanup_evidence
"""Approved original E + sealed wrk staircase; diagnostic buffers/client are never loaded."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import signal
import sys
import time

from observed_sample import resource_gate
from wire_types import identity, write_json

BASE=Path(__file__).resolve().parents[2]
ROLE=BASE/'.cache/v0.5.1-s4/builder'
ORDER=(8,32,64,128,128,64,32,8)
SOURCE=ROLE/'E-S3/source'
INPUTS={
    str(BASE/'benchmark/tail-localization/observed_sample.py'):'ec3539f08f07a00b3aebf1f2fdfdb5e9b09ca2570ed65f0a0be2d64b79f607f9',
    str(BASE/'benchmark/tail-localization/wire_types.py'):'fc03ca43c6bf93a605751179d6a38856cf30e0886213306571ddbbf853a553e5',
    str(ROLE/'build-E/hp_http_server'):'d5053e5697cf84df96ddf5b48b581db27dc2eb9124c32743b76772af87a53ce0',
    str(BASE/'.cache/v0.5-s4/tools/root/usr/bin/wrk'):'b10e53769443c2bf3be2cdedec8ef6571aa5bfd1494796b247f3f9296e3af71d',
    str(SOURCE/'benchmark/run.py'):'9fb01e7b2da9b7d4c0f46546d1f92b3fd2506301fdb85c9febb00be436b081ba',
    str(SOURCE/'benchmark/build.py'):'32340a0c73946ad18938b0c48247a18f44eb8c297d495961a710fac8bea8e22b',
    str(SOURCE/'benchmark/regression/executor.py'):'93fe1c1794df2c601602f6d6b9e2a7d0a26223e281c5b4e930281e50e1d35e24',
    str(SOURCE/'benchmark/regression/identity.py'):'128fbd135d5194b4841a256dc24a96b0f22ba8cd0bf9f1383ba06dc8b3d104c9',
    str(SOURCE/'benchmark/regression/model.py'):'3abfa1d79b728556bd8a3e713da6a390c8cff33c5a7f0177ccdd2e2ae74a8f39',
    str(SOURCE/'benchmark/summary.lua'):'0ded2fdca3ab1f53274ae70806117c3c6288fec1bb05b0a0b601d55bf36b73ea',
    str(BASE/'.cache/v0.5-s4/tools/root/usr/lib/x86_64-linux-gnu/libluajit-5.1.so.2.1.1703358377'):'04c8976ae97b5e89ba27c008bb9d43c002bfc5b1f923c0598953df278ae14bfe'}


def require(condition,message):
    if not condition:raise RuntimeError(message)


def sealed_inputs():
    for path,digest in INPUTS.items():
        require(hashlib.sha256(Path(path).read_bytes()).hexdigest()==digest,'original staircase input drift: '+path)


def sample(number):
    require(1<=number<=8 and os.getuid()==1000 and os.getgid()==1000,'original native power staircase contract')
    run_id=f'run-staircase-{number:02d}'
    output=ROLE/run_id
    require(output.is_dir() and not any(path.is_symlink() for path in (output,*output.parents)),'not admitted run root')
    current,index=admit('builder',output,'staircase')
    require(index==number-1,'staircase number differs')
    deadline=current['start_monotonic']+45-7
    sealed_inputs()
    original_env=dict(LD_LIBRARY_PATH=str(BASE/'.cache/v0.5-s4/tools/root/usr/lib/x86_64-linux-gnu'),
                      LUA_PATH=str(BASE/'.cache/v0.5-s4/tools/root/usr/share/luajit-2.1/?.lua')+';;')
    os.environ.update(original_env)
    result=dict(status='invalid',number=number,connections=ORDER[number-1],warmup_seconds=5,measurement_seconds=20,
                input_sha256=INPUTS,route=dict(identity=identity(os.getpid()),uid=os.getuid(),gid=os.getgid(),groups=os.getgroups(),
                    affinity=sorted(os.sched_getaffinity(0)),nofile=list(resource.getrlimit(resource.RLIMIT_NOFILE)),
                    environment={name:os.environ.get(name) for name in ('TMPDIR','TMP','TEMP','XDG_CACHE_HOME','LD_LIBRARY_PATH','LUA_PATH')}),
                diagnostic_recording=False,limits=['original wrk corrected histogram; request-level raw distribution unavailable',
                    'two samples per connection count do not establish statistical significance or a CPU capacity limit'])
    def expired(number,frame):raise TimeoutError('staircase work deadline/signal')
    saved={sig:signal.signal(sig,expired) for sig in (signal.SIGALRM,signal.SIGTERM,signal.SIGINT)}
    signal.setitimer(signal.ITIMER_REAL,max(.001,deadline-time.monotonic()))
    try:
        result['resources_before_start']=resource_gate(output,'before-start')
        # Load original sealed benchmark primitives from their fixed source export, without CLI/build mutation.
        sys.path.insert(0,str(SOURCE/'benchmark/regression'))
        import executor
        adapt_executor(executor,deadline)
        from identity import legacy
        root=output/'root';root.mkdir()
        payload=legacy.fixture(root,1024)
        require(payload['sha256']=='785b0751fc2c53dc14a4ce3d800e69ef9ce1009eb327ccf458afe09c242c26c9','original payload drift')
        manifest=dict(label='E-S3',commit='acda3f92d42a36d0b0554e185bc6f4155b4e5889',
                      binary=str(ROLE/'build-E/hp_http_server'),binary_sha256=INPUTS[str(ROLE/'build-E/hp_http_server')])
        item=dict(scenario='S4-staircase',workers=4,threads=2,connections=ORDER[number-1],size=1024,label='E-S3')
        result['sample']=executor.run_sample(manifest,str(BASE/'.cache/v0.5-s4/tools/root/usr/bin/wrk'),root,payload,
                                            output/'sample',output,deadline,item,warmup=5,duration=20)
        sealed_inputs()
        require(result['sample']['status']=='valid','original sample invalid')
        result['status']='valid'
    except BaseException as error:result['error']=repr(error)
    finally:
        signal.setitimer(signal.ITIMER_REAL,0)
        cleanup_errors=[]
        for sig,handler in saved.items():cleanup_step(cleanup_errors,'handler',lambda sig=sig,handler=handler:signal.signal(sig,handler))
        if cleanup_errors:result.update(status='invalid',cleanup_errors=cleanup_errors)
        if not cleanup_evidence(output/'staircase.json',result,write_json,cleanup_errors):result['status']='invalid'
    return 0 if result['status']=='valid' else 1


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--number',type=int,required=True)
    raise SystemExit(sample(parser.parse_args().number))
