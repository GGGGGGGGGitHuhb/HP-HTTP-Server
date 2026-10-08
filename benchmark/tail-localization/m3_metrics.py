"""Finite identity-checked resource samples and trusted wrk summary validation."""
import errno
import json
import math
import os
from pathlib import Path
import resource
import re
import time


def process_metrics(owner):
    pid=owner['pid']
    try:
        raw=Path(f'/proc/{pid}/stat').read_text();fields=raw[raw.rfind(')')+2:].split()
        if int(fields[19])!=owner['starttime']:raise ValueError('metric PID/starttime changed')
        cpu=(int(fields[11])+int(fields[12]))/os.sysconf('SC_CLK_TCK')
        status=Path(f'/proc/{pid}/status').read_text()
        rss=[line.split() for line in status.splitlines() if line.startswith('VmRSS:')]
        if len(rss)!=1 or rss[0][2]!='kB':raise ValueError('metric RSS unavailable')
        # Recheck ownership after the two reads; PID reuse is never accepted.
        again=Path(f'/proc/{pid}/stat').read_text();again=again[again.rfind(')')+2:].split()
        if int(again[19])!=owner['starttime']:raise ValueError('metric identity changed during read')
        return dict(pid=pid,starttime=owner['starttime'],time_ns=time.monotonic_ns(),cpu_seconds=cpu,rss_kib=int(rss[0][1]))
    except OSError as error:
        if error.errno in (errno.ENOENT,errno.ESRCH):return None
        error.target_pid=pid;raise


def children_cpu():
    usage=resource.getrusage(resource.RUSAGE_CHILDREN)
    return usage.ru_utime+usage.ru_stime


def summary(text,returncode):
    if returncode!=0:raise ValueError('wrk nonzero exit')
    lines=[line[len('BENCH_SUMMARY '):] for line in text.splitlines() if line.startswith('BENCH_SUMMARY ')]
    if len(lines)!=1:raise ValueError('missing or duplicate trusted summary')
    row=json.loads(lines[0])
    if row['schema']!=1:raise ValueError('summary schema differs')
    for name in ('duration_us','requests','bytes'):
        value=row[name]
        if type(value) not in (int,float) or not math.isfinite(value) or value<0 or int(value)!=value:raise ValueError('summary count invalid')
    if row['requests']<=0 or not 16e6<=row['duration_us']<=23e6 or row['bytes']<1024*row['requests']:raise ValueError('summary duration/body/count invalid')
    for name in ('connect','read','write','status','timeout'):
        if type(row['errors'][name]) not in (int,float) or row['errors'][name]!=0:raise ValueError('wrk reported errors')
    latency=row['latency_us']
    for name in ('mean','p50','p95','p99','max'):
        value=latency[name]
        if type(value) not in (int,float) or not math.isfinite(value) or value<0:raise ValueError('latency invalid')
    if not latency['p50']<=latency['p95']<=latency['p99']<=latency['max'] or latency['mean']>latency['max']:raise ValueError('latency order invalid')
    raw_lines=[line for line in text.splitlines() if line.startswith('S4 raw latency us:')]
    if len(raw_lines)!=1:raise ValueError('missing or duplicate global raw latency line')
    match=re.fullmatch(r'S4 raw latency us: count=(\d+) p50=(\d+) p99=(\d+) max=(\d+)',raw_lines[0])
    if match is None:raise ValueError('malformed global raw latency line')
    count,p50,p99,maximum=map(int,match.groups())
    if count!=row['requests'] or not 0<=p50<=p99<=maximum<=2_000_000:raise ValueError('global raw histogram count/range differs')
    return dict(**row,qps=row['requests']/(row['duration_us']/1e6),corrected_latency_ms={k:v/1000 for k,v in latency.items()},raw_latency_available=True,raw_latency_ms=dict(count=count,p50=p50/1000,p99=p99/1000,max=maximum/1000),raw_scope='all 128 connections; completion within measurement window; warmup excluded')


def finish_resources(samples,server_final,client_total_cpu,measurement_start_ns):
    if not samples or server_final is None:raise ValueError('missing measurement resources')
    first=samples[0];elapsed=(server_final['time_ns']-first['server']['time_ns'])/1e9
    client_cpu=client_total_cpu-first['client']['cpu_seconds']
    server_cpu=server_final['cpu_seconds']-first['server']['cpu_seconds']
    if elapsed<=0 or min(client_cpu,server_cpu)<0:raise ValueError('resource counter/time reversed')
    return dict(measurement_start_ns=measurement_start_ns,observed_start_ns=first['server']['time_ns'],observed_end_ns=server_final['time_ns'],envelope_seconds=elapsed,server_cpu_seconds=server_cpu,client_cpu_seconds=client_cpu,server_cpu_percent_one_core=server_cpu/elapsed*100,client_cpu_percent_one_core=client_cpu/elapsed*100,client_final_source='RUSAGE_CHILDREN after successful client poll reap, before server stop/reap',client_reap_observed_ns=time.monotonic_ns(),server_final_source='verified live /proc before server stop',server_rss_sampled_max_kib=max(x['server']['rss_kib'] for x in samples),client_rss_sampled_max_kib=max(x['client']['rss_kib'] for x in samples),samples=samples,limits=['RSS is sampled maximum; no exact peak claim','CPU interval begins at first identity-checked measurement sample','client final CPU comes from reaped child rusage; server is still alive'])
