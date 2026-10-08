"""R012 pure entry-only three-slot window; successful/semantic failures stop."""
import json
import math
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]

def admit(role,output):
    if role not in ('builder','reviewer'):raise ValueError('unknown pure role')
    role_root=ROOT/'.cache/v0.5.1-s4'/role;output=Path(output).absolute()
    slots=('run-m3-pure-002','run-m3-pure-003','run-m3-pure-004') if role=='builder' else ('run-m3-pure-002',)
    if output.parent!=role_root or output.name not in slots or not output.is_dir() or any(p.is_symlink() for p in (output,*output.parents)):raise ValueError('pure own exact slot required')
    records=json.loads((role_root/'ledger.json').read_text())['runs'];active=[r for r in records if r['status']=='running']
    if len(active)!=1 or active[0]['run_id']!=output.name or active[0]['kind']!='selfcheck' or active[0]['reserved_seconds']!=20 or active[0]['output']!=str(output):raise ValueError('pure unique20 reservation required')
    for row in records:
        if row is active[0]:continue
        charge=row.get('charged_seconds')
        if row['status'] not in ('valid','invalid') or isinstance(charge,bool) or not isinstance(charge,(int,float)) or not math.isfinite(charge) or charge<0:raise ValueError('pure prior charge unresolved')
    window=[r for r in records if r['run_id'] in slots];index=slots.index(output.name)
    if [r['run_id'] for r in window]!=list(slots[:index+1]):raise ValueError('pure window skipped/duplicate/order')
    if sum(r['charged_seconds'] for r in window[:-1])+20>60:raise ValueError('pure60 exceeded')
    for row in window[:-1]:
        if row['status']!='invalid' or row.get('kind')!='selfcheck' or row.get('reserved_seconds')!=20 or row.get('output')!=str(role_root/row['run_id']) or row.get('accounting_errors')!=[] or row.get('byte_classification_status')!='verified':raise ValueError('pure prior success or unsafe failure')
        directory=role_root/row['run_id'];cleanup=json.loads((directory/'cleanup.json').read_text());protection=json.loads((directory/'protection-after.json').read_text());failure=json.loads((directory/'pure-failure.json').read_text())
        if not cleanup['complete'] or cleanup['forced'] or cleanup['errors'] or cleanup['remaining'] or cleanup.get('unknown') or not protection['match'] or failure['category']!='entry' or failure['type'] not in ('SyntaxError','IndentationError','ModuleNotFoundError','FileNotFoundError'):raise ValueError('pure semantic/resource/cleanup failure stops window')
    return active[0]
