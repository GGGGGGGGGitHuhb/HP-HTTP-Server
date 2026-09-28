#!/usr/bin/env python3
"""S2 bounded C/D syscall and default-client receive timeline."""
import argparse
import ctypes
import subprocess
import json
import pathlib
import socket
import statistics
import time
import repair as diag


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
        return diag.bench.OwnedProcess(command, prefix)
    finally:
        subprocess.Popen = original


def receive_series(port, payload, deadline):
    rows=[]
    with socket.create_connection(('127.0.0.1',port),timeout=1) as peer:
        peer.settimeout(1)
        for index in range(32):
            diag.bench.demand(time.monotonic()<deadline,'10 second group budget')
            began=time.time_ns()
            peer.sendall(f'GET /{payload["name"]} HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n'.encode())
            received=b''
            while b'\r\n\r\n' not in received:
                part=peer.recv(65536)
                diag.bench.demand(bool(part),'early EOF headers')
                received+=part
            header_at=time.time_ns()
            header,body=received.split(b'\r\n\r\n',1)
            diag.bench.demand(header.startswith(b'HTTP/1.1 200 '),'bad status')
            while len(body)<payload['size']:
                part=peer.recv(65536)
                diag.bench.demand(bool(part),'early EOF body')
                body+=part
            ended=time.time_ns()
            diag.bench.demand(len(body)==payload['size'] and diag.hashlib.sha256(body).hexdigest()==payload['sha256'],'bad body/tail')
            rows.append(dict(request=index,send_ns=began,header_ns=header_at,body_ns=ended,
                             header_ms=(header_at-began)/1e6,body_wait_ms=(ended-header_at)/1e6))
    return rows


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--role',choices=('builder','reviewer'),required=True)
    p.add_argument('--label',choices=('C','D'),required=True)
    p.add_argument('--output',required=True)
    a=p.parse_args()
    output=diag.new_output(a.role,a.output)
    role=diag.role_root(a.role)
    started=time.monotonic()
    prior=diag.budget_seconds(role)
    result=dict(kind='trace',status='invalid',
                hypothesis='server accepted-socket NODELAY removes body wait with default client ACK policy',
                changed_factor='C baseline versus D server NODELAY; client socket defaults',
                falsifier='any D connection median body wait reaches 5ms',
                label=a.label,client_socket_options="default",started_utc=diag.bench.utc(),prior_dynamic_seconds=prior,
                tools=diag.tool_identity(),timeline_sha256=diag.builds.sha(__file__),series=[])
    diag.bench.save(output/'run.json',result)
    server=tracer=None
    try:
        result['environment']=diag.bench.environment(output)
        diag.bench.demand(prior<1790,'role dynamic budget')
        diag.require_mechanism_budget(role)
        manifest=diag.validate_manifest(role/a.label/'manifest.json',a.label)
        result['manifest']=manifest
        fixture=output/'root';fixture.mkdir()
        payload=diag.bench.fixture(fixture,1024)
        server=start_traceable_server([manifest['binary'],*diag.bench.SERVER_ARGS,'--root',str(fixture)],output/'server')
        port=diag.bench.ready(server,output,started+3)
        result['port']=port
        result['pre_audit']=diag.bench.audit(port,payload)
        tracer=diag.bench.OwnedProcess(['strace','-f','-ttt','-T','-e','trace=read,readv,recvfrom,sendto,sendfile,epoll_wait,epoll_ctl,setsockopt','-p',str(server.process.pid),'-o',str(output/'syscalls.trace')],output/'tracer')
        time.sleep(.15)
        diag.bench.demand(tracer.alive(),'strace attach failed')
        for repeat in range(3):
            result['series'].append(receive_series(port,payload,started+9))
            diag.guard(role,prior,started)
            trace_bytes=sum(p.stat().st_size for p in role.rglob('*.trace'))
            diag.bench.demand(trace_bytes+diag.bench.log_bytes(role)<diag.bench.LOG_LIMIT,'trace/log cumulative budget')
        result['post_audit']=diag.bench.audit(port,payload)
        diag.bench.demand(diag.validate_manifest(role/a.label/'manifest.json',a.label)==manifest,'manifest identity drift')
        diag.bench.demand(diag.tool_identity()==result['tools'] and diag.builds.sha(__file__)==result['timeline_sha256'],'diagnostic tool identity drift')
        result['body_wait_medians_ms']=[statistics.median(row['body_wait_ms'] for row in series) for series in result['series']]
        if a.label=='D':
            diag.bench.demand(all(value<5 for value in result['body_wait_medians_ms']),'D body wait threshold failed')
        diag.guard(role,prior,started)
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
        result['ended_utc']=diag.bench.utc()
        diag.bench.save(output/'run.json',result)
    print(a.label,result['status'],result.get('body_wait_medians_ms',[]),result.get('error',''),flush=True)
    return 0 if result['status']=='valid' else 1

if __name__=='__main__':raise SystemExit(main())
