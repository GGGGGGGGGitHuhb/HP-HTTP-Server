"""Actual startup-process whitelist evidence, once per root/power process."""
import hashlib
import json
import os
from pathlib import Path
from wire_types import identity

ROOT=Path(__file__).resolve().parents[2]
NAMES=('TMPDIR','TMP','TEMP','XDG_CACHE_HOME','PYTHONDONTWRITEBYTECODE','LD_LIBRARY_PATH','LUA_PATH')

def expected(role,manifest):
    if role not in ('builder','reviewer') or manifest.get('role')!=role:
        raise ValueError('environment role/manifest differs')
    role_root=ROOT/'.cache/v0.5.1-s4'/role
    return dict(TMPDIR=str(role_root/'tmp'),TMP=str(role_root/'tmp'),TEMP=str(role_root/'tmp'),
        XDG_CACHE_HOME=str(role_root/'cache'),PYTHONDONTWRITEBYTECODE='1',
        LD_LIBRARY_PATH=manifest['runtime']['library_directory'],LUA_PATH=manifest['runtime']['lua_path'])

def manifest_sha(manifest):
    return hashlib.sha256(json.dumps(manifest,sort_keys=True,separators=(',',':')).encode()).hexdigest()

def capture(role,run,phase,manifest):
    values=expected(role,manifest)
    owner=identity(os.getpid())
    return dict(schema=1,role=role,run_id=run,phase=phase,identity=owner,
        pid=os.getpid(),starttime=owner['starttime'],uid=os.getuid(),gid=os.getgid(),
        manifest_sha256=manifest_sha(manifest),
        environment={name:dict(present=name in os.environ,matches_expected=os.environ.get(name)==values[name],
                    value=values[name] if os.environ.get(name)==values[name] else None) for name in NAMES})

def validate(evidence,role,run,phase,manifest,owner,uid,gid):
    if evidence.get('schema')!=1 or (evidence.get('role'),evidence.get('run_id'),evidence.get('phase'),evidence.get('uid'),evidence.get('gid'))!=(role,run,phase,uid,gid):
        raise ValueError('startup environment role/run/phase/identity differs')
    if evidence.get('pid')!=owner['pid'] or evidence.get('starttime')!=owner['starttime'] or evidence.get('identity',{}).get('pid')!=owner['pid'] or evidence['identity'].get('starttime')!=owner['starttime']:
        raise ValueError('startup environment PID/starttime differs')
    if evidence.get('manifest_sha256')!=manifest_sha(manifest):
        raise ValueError('startup environment manifest differs')
    values=expected(role,manifest)
    if set(evidence.get('environment',{}))!=set(NAMES):raise ValueError('startup whitelist differs')
    for name,value in values.items():
        row=evidence['environment'][name]
        if row.get('present') is not True or row.get('matches_expected') is not True or row.get('value')!=value:
            raise ValueError('startup environment mismatch: '+name)
    return evidence

def publish(output,role,phase,manifest,owner,uid,gid):
    output=Path(output)
    evidence=capture(role,output.name,phase,manifest)
    path=output/('root-environment.json' if uid==0 else 'power-environment.json')
    # Mismatch evidence is retained; validation still prevents resource creation/GO.
    with path.open('x') as stream:json.dump(evidence,stream,indent=2);stream.write('\n')
    return validate(evidence,role,output.name,phase,manifest,owner,uid,gid)
