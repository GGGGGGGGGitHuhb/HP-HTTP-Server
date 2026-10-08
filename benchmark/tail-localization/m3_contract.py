"""Exact original M3 sample admission; no workload or ledger mutations."""
import json
import hashlib
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
STAIRS=tuple(f'run-staircase-{n:02d}' for n in range(1,9))
ABBA=tuple(f'run-m3-abba-{n:02d}' for n in range(1,5))
CONFIRM=tuple(f'run-m3-confirm-{n:02d}' for n in range(1,3))


def require(value,message):
    if not value:raise ValueError(message)


def admit(role,output,kind):
    require(role in ('builder','reviewer'),'unknown M3 role')
    output=Path(output).absolute();role_root=ROOT/'.cache/v0.5.1-s4'/role
    require(output.parent==role_root and output.is_dir() and not any(p.is_symlink() for p in (output,*output.parents)),'exact ordinary M3 role output required')
    sequence=STAIRS if kind=='staircase' and role=='builder' else ABBA if kind=='observed_abba' and role=='builder' else CONFIRM if kind=='confirmation_ab' and role=='reviewer' else ()
    require(output.name in sequence,'unapproved M3 run/kind/role')
    ledger=json.loads((role_root/'ledger.json').read_text());records=ledger['runs'];selected=[r for r in records if r['kind']==kind]
    number=sequence.index(output.name)
    require([r['run_id'] for r in selected]==list(sequence[:number+1]),'M3 run duplicate, skipped or out of order')
    require(all(r['status']=='valid' for r in selected[:-1]),'previous M3 sample failed')
    current=selected[-1];running=[r for r in records if r['status']=='running']
    require(len(running)==1 and running[0] is current and current['status']=='running' and current['reserved_seconds']==45 and current['output']==str(output),'unique 45 second M3 reservation required')
    if kind=='observed_abba':
        stairs=[r for r in records if r['kind']=='staircase']
        require([r['run_id'] for r in stairs]==list(STAIRS) and all(r['status']=='valid' for r in stairs),'all original staircase samples must precede ABBA')
    if kind in ('observed_abba','confirmation_ab') and number:
        previous=sequence[number-1];offline_id='run-m3-offline-'+previous.removeprefix('run-m3-')
        offline=[r for r in records if r['run_id']==offline_id]
        require(len(offline)==1 and offline[0]['status']=='valid' and offline[0]['kind']=='offline','previous M3 offline result missing or failed')
        directory=role_root/previous
        receipt_path=directory/'association.json';sample_path=directory/'sample.json'
        require(not any(p.is_symlink() for p in (directory,receipt_path,sample_path,*directory.parents)),'previous M3 evidence symlink')
        receipt=json.loads(receipt_path.read_text())
        require(receipt['status']=='valid' and receipt['run_id']==previous and receipt['role']==role and receipt['sample_sha256']==hashlib.sha256(sample_path.read_bytes()).hexdigest(),'previous M3 receipt drift')
        require(receipt['overflow'] is False and receipt['kernel_mapping_verified'] is True and receipt['selected_connections']==4 and not receipt['cleanup']['forced'] and not receipt['cleanup']['errors'] and not receipt['cleanup']['remaining'] and not receipt['cleanup'].get('unknown'),'previous M3 association incomplete')
    return current,number


def admit_observed(args):
    kind='observed_abba' if args.role=='builder' else 'confirmation_ab'
    record,index=admit(args.role,args.output,kind)
    expected_detailed=(False,True,True,False)[index] if args.role=='builder' else (False,True)[index]
    require((args.connections,args.requests_per_connection,args.warmup,args.duration,args.detailed)==(128,0,5,20,expected_detailed),'original M3 configuration differs')
    return record
