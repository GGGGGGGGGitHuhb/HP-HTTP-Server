"""Static explicit-role input seal; never invokes binaries or modifies old manifests."""
import argparse
import hashlib
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
TOOLS=Path(__file__).resolve().parent


def seal(path):
    path=Path(path).absolute()
    if not path.is_file() or any(item.is_symlink() for item in (path,*path.parents)):
        raise ValueError('manifest input is not an ordinary native file')
    return dict(path=str(path),sha256=hashlib.sha256(path.read_bytes()).hexdigest())


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--role',choices=('builder','reviewer'),required=True)
    parser.add_argument('--server',required=True);parser.add_argument('--client',required=True);parser.add_argument('--summary',required=True);parser.add_argument('--output',required=True)
    args=parser.parse_args();role=ROOT/'.cache/v0.5.1-s4'/args.role;output=Path(args.output).absolute()
    if not output.is_relative_to(role) or output.exists():raise ValueError('new own-role manifest required')
    legacy=json.loads((TOOLS/'m2-inputs-002.json').read_text())
    result=dict(schema=3,role=args.role,baseline_commit='acda3f92d42a36d0b0554e185bc6f4155b4e5889',runtime=legacy['runtime'],payload=legacy['payload'])
    for name in ('server','client','summary'):
        path=Path(getattr(args,name)).absolute()
        if not path.is_relative_to(role):raise ValueError('cross-role binary or summary forbidden')
        result[name]=seal(path)
    result['tools']={name:seal(TOOLS/name) for name in ('root_marker_launcher_v4.py','observed_sample_v4.py','process_local_launcher_v4.py','wire_types_v3.py','control_protocol_v3.py','decode_v3.py','budget_v10.py','localize_v11.py','watchdog_v11.py','cleanup_v11.py','proc_identity_v11.py','inner_identity_v11.py','wire_types.py','protection.py')}
    result['diagnostic_sources']={name:seal(TOOLS/name) for name in ('server/ObservedRuntimeV3.cpp','server/ObservedRuntimeV3.h','server/patch_server_v3.py','client/patch_client_v3.py','client/ClientObserverV3.c','client/ClientObserverV3.h','common/TailWireV3.h','common/StartupEvidence.h')}
    output.write_text(json.dumps(result,indent=2)+'\n')


if __name__=='__main__':main()
