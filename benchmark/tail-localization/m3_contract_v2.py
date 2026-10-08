"""R012 exact old-failure exception and effective original ABBA order."""
import hashlib
import json
from pathlib import Path
from m3_contract import ROOT,STAIRS,CONFIRM,require

ABBA=('run-m3-abba-01-repair-001','run-m3-abba-02','run-m3-abba-03','run-m3-abba-04')

def check_history(binding):
    builder=ROOT/'.cache/v0.5.1-s4/builder'
    ledger=json.loads((builder/'ledger.json').read_text())
    for item in binding['records']:
        found=[row for row in ledger['runs'] if row['run_id']==item['run_id']]
        require(len(found)==1 and hashlib.sha256(json.dumps(found[0],sort_keys=True,separators=(',',':')).encode()).hexdigest()==item['sha256'],'R012 canonical history drift')
    old=next(row for row in ledger['runs'] if row['run_id']=='run-m3-abba-01')
    require(old['status']=='invalid' and old['kind']=='observed_abba' and old['accounting_errors']==[],'R012 exception is not exact settled old failure')
    for item in binding['files']:
        path=Path(item['path'])
        require(path.is_absolute() and '..' not in path.parts and not any(p.is_symlink() for p in (path,*path.parents)),'history path unsafe')
        require(path.exists()==item['present'] and (not item['present'] or hashlib.sha256(path.read_bytes()).hexdigest()==item['sha256']),'R012 old artifact drift')
    audit_row=next(row for row in ledger['runs'] if row['run_id']=='run-m3-resource-audit-002')
    require(audit_row['status']=='valid' and audit_row['kind']=='selfcheck' and audit_row['accounting_errors']==[],'audit002 outer not valid')
    audit=json.loads((builder/'run-m3-resource-audit-002/resource-audit.json').read_text())
    require(audit['conclusion']=='verified_safe' and all(audit['conditions'].values()),'resource prerequisite missing')
    return True

def admit(role,output,kind,binding):
    require(role in ('builder','reviewer'),'unknown M3 role')
    check_history(binding)
    output=Path(output).absolute();role_root=ROOT/'.cache/v0.5.1-s4'/role
    require(output.parent==role_root and output.is_dir() and not any(p.is_symlink() for p in (output,*output.parents)),'exact role output required')
    sequence=ABBA if role=='builder' and kind=='observed_abba' else CONFIRM if role=='reviewer' and kind=='confirmation_ab' else ()
    require(output.name in sequence,'unapproved effective run/kind')
    rows=json.loads((role_root/'ledger.json').read_text())['runs'];selected=[r for r in rows if r['kind']==kind]
    index=sequence.index(output.name)
    expected=(['run-m3-abba-01'] if role=='builder' else [])+list(sequence[:index+1])
    require([r['run_id'] for r in selected]==expected,'failed exception/order/duplicate differs')
    effective=selected[1:] if role=='builder' else selected
    require(all(r['status']=='valid' for r in effective[:-1]),'previous effective sample failed')
    running=[r for r in rows if r['status']=='running'];current=effective[-1]
    require(len(running)==1 and running[0] is current and current['reserved_seconds']==45 and current['output']==str(output),'unique45 reservation differs')
    if role=='builder':
        stairs=[r for r in rows if r['kind']=='staircase']
        require([r['run_id'] for r in stairs]==list(STAIRS) and all(r['status']=='valid' for r in stairs),'original staircase incomplete')
    if index:
        previous=sequence[index-1];offline='run-m3-offline-'+previous.removeprefix('run-m3-')
        receipts=[r for r in rows if r['run_id']==offline]
        require(len(receipts)==1 and receipts[0]['status']=='valid' and receipts[0]['kind']=='offline','previous offline incomplete')
        directory=role_root/previous
        from m3_compare_v2 import verify_binding
        receipt=verify_binding(directory,json.loads((directory/'association.json').read_text()),role,previous)
        require(not receipt['overflow'] and receipt['kernel_mapping_verified'] is True and receipt['selected_connections']==4 and not receipt['cleanup']['forced'] and not receipt['cleanup']['errors'] and not receipt['cleanup']['remaining'] and not receipt['cleanup'].get('unknown'),'previous association incomplete')
    return current,index

def admit_observed(args):
    manifest=json.loads(Path(args.manifest).read_text())
    row,index=admit(args.role,args.output,'observed_abba' if args.role=='builder' else 'confirmation_ab',manifest['r012_history'])
    detailed=(False,True,True,False)[index] if args.role=='builder' else (False,True)[index]
    require((args.connections,args.requests_per_connection,args.warmup,args.duration,args.detailed)==(128,0,5,20,detailed),'original configuration differs')
    return row
