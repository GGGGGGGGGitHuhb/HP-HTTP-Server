#!/usr/bin/env python3
"""Bounded server syscall/client receive timeline and QUICKACK counterfactual."""
import argparse
import ctypes
import subprocess
import json
import pathlib
import socket
import time
import diagnose as diag
import intermediate


def authorize_tracee_debugging():
    # 仅本次子进程允许同用户 ptracer；不修改系统 Yama 配置。
    libc = ctypes.CDLL(None, use_errno=True)
    libc.prctl.argtypes = [ctypes.c_int, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong]
    if libc.prctl(0x59616d61, ctypes.c_ulong(-1).value, 0, 0, 0) != 0:
        raise OSError(ctypes.get_errno(), 'PR_SET_PTRACER failed')


def start_traceable_server(command, prefix):
    original = subprocess.Popen
    def launch(command, **options):
        return original(command, preexec_fn=authorize_tracee_debugging, **options)
    subprocess.Popen = launch
    try:
        return diag.legacy.OwnedProcess(command, prefix)
    finally:
        subprocess.Popen = original


def receive_series(port, payload, quickack, deadline):
    rows=[]
    with socket.create_connection(('127.0.0.1',port),timeout=1) as peer:
        peer.settimeout(1)
        for index in range(32):
            diag.legacy.demand(time.monotonic()<deadline,'10 second group budget')
            began=time.time_ns()
            peer.sendall(f'GET /{payload["name"]} HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n'.encode())
            received=b''
            while b'\r\n\r\n' not in received:
                part=peer.recv(65536)
                diag.legacy.demand(bool(part),'early EOF headers')
                received+=part
            header_at=time.time_ns()
            header,body=received.split(b'\r\n\r\n',1)
            diag.legacy.demand(header.startswith(b'HTTP/1.1 200 '),'bad status')
            if quickack:
                peer.setsockopt(socket.IPPROTO_TCP,socket.TCP_QUICKACK,1)
            while len(body)<payload['size']:
                part=peer.recv(65536)
                diag.legacy.demand(bool(part),'early EOF body')
                body+=part
            ended=time.time_ns()
            diag.legacy.demand(len(body)==payload['size'] and diag.hashlib.sha256(body).hexdigest()==payload['sha256'],'bad body/tail')
            rows.append(dict(request=index,send_ns=began,header_ns=header_at,body_ns=ended,
                             header_ms=(header_at-began)/1e6,body_wait_ms=(ended-header_at)/1e6))
    return rows


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--role',choices=('builder','reviewer'),required=True)
    p.add_argument('--label',choices=('A','B','C','S1'),required=True)
    p.add_argument('--quickack',action='store_true')
    p.add_argument('--output',required=True)
    a=p.parse_args()
    output=diag.checked_output(a.role,a.output)
    role=diag.role_root(a.role)
    started=time.monotonic()
    prior=diag.accumulated_seconds(role)
    result=dict(kind='experiment' if a.quickack else 'trace',status='invalid',
                hypothesis='split header/body plus delayed ACK creates body wait; QUICKACK alone should remove wait while server syscall pattern remains',
                changed_factor='client TCP_QUICKACK=1 after headers' if a.quickack else 'none: client defaults',
                falsifier='body wait persists with QUICKACK, or server sendfile itself is delayed/blocked',
                label=a.label,quickack=a.quickack,started_utc=diag.legacy.utc(),prior_dynamic_seconds=prior,
                tools=diag.identities(),timeline_sha256=diag.builds.sha(__file__),series=[])
    diag.legacy.save(output/'run.json',result)
    server=tracer=None
    try:
        diag.legacy.environment(output)
        diag.legacy.demand(prior<1790,'role dynamic budget')
        diag.legacy.demand(len(list(role.glob('run-timeline-*/run.json')))<=6,'six timeline group limit')
        manifest=diag.legacy.validate_manifest(role/a.label/'manifest.json',a.label)
        result['manifest']=manifest
        fixture=output/'root';fixture.mkdir()
        payload=diag.legacy.fixture(fixture,1024)
        server=start_traceable_server([manifest['binary'],*diag.legacy.SERVER_ARGS,'--root',str(fixture)],output/'server')
        port=diag.legacy.ready(server,output,started+3)
        result['port']=port
        result['pre_audit']=diag.legacy.audit(port,payload)
        tracer=diag.legacy.OwnedProcess(['strace','-f','-ttt','-T','-e','trace=read,readv,recvfrom,sendto,sendfile,epoll_wait,epoll_ctl,setsockopt','-p',str(server.process.pid),'-o',str(output/'syscalls.trace')],output/'tracer')
        time.sleep(.15)
        diag.legacy.demand(tracer.alive(),'strace attach failed')
        for repeat in range(3):
            result['series'].append(receive_series(port,payload,a.quickack,started+9))
            diag.legacy.log_guard(role)
            trace_bytes=sum(p.stat().st_size for p in role.rglob('*.trace'))
            diag.legacy.demand(trace_bytes+diag.legacy.log_bytes(role)<diag.legacy.LOG_LIMIT,'trace/log cumulative budget')
        result['post_audit']=diag.legacy.audit(port,payload)
        diag.legacy.validate_manifest(role/a.label/'manifest.json',a.label)
        diag.legacy.demand(diag.identities()==result['tools'] and diag.builds.sha(__file__)==result['timeline_sha256'],'diagnostic tool identity drift')
        result['status']='valid'
    except (Exception,KeyboardInterrupt) as error:
        result['error']=repr(error)
    finally:
        if tracer:
            result['tracer_cleanup']=tracer.close(timeout=.5)
            if result['tracer_cleanup']['forced']:
                result['status']='invalid';result['error']='tracer required forced cleanup'
        if server:
            result['server_cleanup']=server.close()
            if result['server_cleanup']['forced'] or result['server_cleanup']['returncode']!=0:
                result['status']='invalid'
        result['wall_seconds']=time.monotonic()-started
        if result['wall_seconds']>10:
            result['status']='invalid';result['error']='10 second group limit'
        if (output/'root').exists():diag.shutil.rmtree(output/'root')
        result['ended_utc']=diag.legacy.utc()
        diag.legacy.save(output/'run.json',result)
    print(a.label,'quickack',a.quickack,result['status'],result.get('error',''),flush=True)
    return 0 if result['status']=='valid' else 1

if __name__=='__main__':raise SystemExit(main())
