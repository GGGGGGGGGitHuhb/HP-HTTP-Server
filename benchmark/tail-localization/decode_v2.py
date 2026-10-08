"""Schema-2 streaming binary merge -> exact request association; no load or live attach."""
import argparse
import errno
from collections import Counter, defaultdict
import hashlib
import heapq
import json
from pathlib import Path
import signal
import struct
from analyze import analyze, Invalid, interval_union

HEADER=struct.Struct('<8sHHIQQIIQQQ')
RECORD=struct.Struct('<QQIIHHi')
CLIENT_KINDS={1,2,3,4,19,20,21,22}
SERVER_KINDS={5,6,7,8,9,10,11,12,13,14,15,16,17,18,23}
PAIRS={5:16,6:7,11:12,13:14,17:18,19:20,21:22}


def demand(value,message):
    if not value:raise Invalid(message)


def validate_payload(kind,value,flags,result):
    """Validate the writer's kind-specific wire contract, before association."""
    if kind==4:
        demand((value,flags,result)==(1024,1,200),'invalid verified completion payload')
    elif kind in (7,12,14):
        demand((flags==0 and result==0) or
               (flags==1 and value==0 and result in errno.errorcode),'invalid server IO return payload')
    elif kind in (20,22):
        demand((flags==0 and result==0) or
               (flags==2 and value==0 and result in (errno.EAGAIN,errno.EWOULDBLOCK)) or
               (flags==1 and value==0 and (result==0 or result in errno.errorcode)),
               'invalid client IO return payload')
    else:
        demand(flags==0 and result==0,'non-return event has status/flags')
        if kind in (15,17,18,23):
            demand(value==0,'scope/lifecycle event has unexpected value')
        elif kind in (5,16):
            demand(value>0 and value & ~(1|4|8|16|8192)==0,'invalid callback readiness mask')
        else:
            demand(value>0,'request/IO event has zero value')
    if kind==21:demand(value==8192,'unexpected client read request size')
    if kind==13:demand(value<=1024,'sendfile request exceeds fixed body')


def thread_records(path, endpoint, expected, sample, catalog):
    digest=hashlib.sha256()
    with path.open('rb') as stream:
        raw=stream.read(HEADER.size);digest.update(raw)
        demand(len(raw)==HEADER.size,'short schema-2 header')
        magic,header_size,record_size,flags,count,capacity,pid,tid,starttime,first,last=HEADER.unpack(raw)
        demand((magic,header_size,record_size,flags)==(b'S4TAIL02',64,32,1),'schema/writer/overflow/clock invalid')
        demand(count<=capacity==16*1024*1024//32 and path.stat().st_size==64+count*32,'count/capacity/file length mismatch')
        demand((pid,tid,starttime)==(expected['pid'],expected['namespace_tid'],expected['starttime']),'header mapped identity differs')
        previous=0;actual_first=actual_last=0;ordinal=0
        while ordinal<count:
            block=stream.read(min(2048,count-ordinal)*32)
            demand(len(block)>0 and len(block)%32==0,'short record block')
            digest.update(block)
            for timestamp,value,connection,sequence,kind,event_flags,result in RECORD.iter_unpack(block):
                demand(timestamp>=previous and sample['recording_start_ns']<=timestamp<=sample['recording_end_ns'],'record clock/order outside coverage')
                demand(kind in (CLIENT_KINDS if endpoint=='client' else SERVER_KINDS) and sequence>0,'unknown kind/endpoint/sequence')
                validate_payload(kind,value,event_flags,result)
                previous=timestamp;actual_first=timestamp if ordinal==0 else actual_first;actual_last=timestamp
                yield (timestamp,endpoint,expected['worker'],ordinal,connection,sequence,kind,value,event_flags,result)
                ordinal+=1
        demand((actual_first,actual_last)==(first,last),'header timestamp bounds mismatch')
        catalog.append(dict(path=str(path),sha256=digest.hexdigest(),records=count,capacity=capacity,pid=pid,namespace_tid=tid,kernel_tid=expected['kernel_tid'],starttime=starttime))


def decode(directory):
    directory=Path(directory)
    sample=json.loads((directory/'sample.json').read_text())
    marker=json.loads((directory/'marker-launcher.json').read_text())
    demand(sample['status']=='valid' and marker['status']=='valid','capture invalid')
    demand(not sample['cleanup']['forced'] and not sample['cleanup']['errors'] and not sample['cleanup']['remaining'],'capture cleanup invalid')
    demand(sample['parameters']['detailed'],'A disabled recording is not a request trace')
    demand(marker.get('marker_lost_events')==0,'startup marker loss unknown/nonzero')
    mappings=sample['kernel_mappings'];selected=sample['frozen_connections']
    demand(0<len(selected)<=16,'selection cap')
    by_thread={(item['endpoint'],item['worker']):item for item in mappings}
    by_connection={}
    connections=[]
    run_id=directory.name
    processes={item['name']:item for item in json.loads((directory/'processes.json').read_text())}
    for index,pair in enumerate(selected):
        owners={}
        for endpoint in ('server','client'):
            record=pair[endpoint];mapping=by_thread[(endpoint,record['worker'])]
            demand((record['pid'],record['tid'],record['starttime'])==(mapping['pid'],mapping['namespace_tid'],mapping['starttime']),'connection/thread mapping drift')
            owners[endpoint]=dict(pid=record['pid'],starttime=processes[endpoint]['starttime'],tid=mapping['kernel_tid'],worker=record['worker'],namespace_tid=record['tid'],thread_starttime=record['starttime'])
            demand((endpoint,record['index']) not in by_connection,'duplicate selected connection')
            by_connection[(endpoint,record['index'])]=index
        connections.append(dict(run_id=run_id,four_tuple=pair['four_tuple'],lifetime=pair['server']['lifetime'],frozen_before_warmup=True,one_inflight=True,reconnect=False,**owners))
    endpoint_clock=[]
    for endpoint in ('server','client'):
        identity=next(item['identity'] for item in mappings if item['endpoint']==endpoint and item['role']=='main')
        endpoint_clock.append(dict(boot_id=identity['boot_id'],time_namespace=identity['time_namespace']))
    demand(len({item['boot_id'] for item in endpoint_clock})==1 and len({item['time_namespace'] for item in endpoint_clock})==1,'endpoint clocks differ')
    catalog=[];streams=[]
    for endpoint,count in [('server',4),('client',2)]:
        for worker in range(count):
            streams.append(thread_records(directory/f'{endpoint}-worker-{worker}.events.bin',endpoint,by_thread[(endpoint,worker)],sample,catalog))
    active={};finalized=defaultdict(int);origins=defaultdict(int);counts=Counter();slow=[];boundaries=[];idle=0;idle_reasons=Counter();complete_count=0;phase_counts=Counter()
    end_states={(item['endpoint'],item['index']):item for item in sample['connection_end_states']}
    def normalize_event(connection,sequence,kind,timestamp,endpoint,extra=None):
        result=dict(run_id=run_id,four_tuple=connection['four_tuple'],lifetime=connection['lifetime'],request_sequence=sequence,kind=kind,time_ns=timestamp,endpoint=endpoint,**connection[endpoint])
        if extra:result.update(extra)
        return result
    def finish(key, group, boundary=False):
        nonlocal complete_count,idle
        connection_index,sequence=key;connection=connections[connection_index]
        points=group['points'];events=[]
        demand(not group['open_calls'],'unclosed syscall/session/callback scope, including idle or pending group')
        if 1 not in points and not group['positive_reads']:
            demand(not any(kind in points for kind in (2,3,4,8,9,10,15)),
                   'request endpoint without origin cannot be classified idle')
            idle+=1;idle_reasons['no_client_origin_no_positive_recv_fully_paired']+=1;return
        # A real request is rooted in the client first write, never an idle callback or corrected bucket.
        demand(1 in points,'positive server read lacks client write origin')
        missing=[kind for kind in (1,2,3,4,8,9,10,15) if kind not in points]
        handler=next((timestamp for timestamp in group['handlers'] if 15 in points and timestamp>=points[15]),None)
        if not group['positive_reads']:missing.append(7)
        if handler is None:missing.append(16)
        if missing:
            client_index=selected[connection_index]['client']['index']
            end=end_states[('client',client_index)]
            last_sequence=end['reserved'] & 0xffffffff
            pending=bool(end['reserved']>>32)
            writer=next(item for item in sample['final_threads'] if item['endpoint']=='client' and item['worker']==connection['client']['worker'])
            demand(boundary and sequence==last_sequence and pending and writer['stoppedNs']>=sample['measurement_end_ns'],'missing interior request endpoints')
            boundaries.append(dict(connection=connection_index,request_sequence=sequence,missing=missing,reason='client_pending_at_measurement_stop',client_stop_ns=writer['stoppedNs'],observed_points=points))
            return
        demand(not group['open_calls'],'unclosed interior syscall/session scope')
        demand(points[1]<=points[2]<=points[3]<=points[4],'client local ordering invalid')
        first_read=group['positive_reads'][0]
        demand(first_read<=points[8]<=points[9]<=points[10]<=points[15]<=handler,'server local ordering invalid')
        demand(group['body']==(1024,200,1),'response body/status verification missing')
        demand(group['sent_bytes']==group['enqueue_bytes'],'server successful output byte accounting differs')
        # A prospective sequence may have completed idle callbacks before its real client origin.
        # Fully returned pre-receive scopes do not establish server request progress,
        # even when the client's write has already started. Raw times remain in the evidence.
        prior_idle={scope for scope in group['intervals'] if scope[0]=='server'
                    and scope[3]<first_read}
        if prior_idle:
            idle_reasons['fully_paired_prospective_scope_before_real_origin']+=len(prior_idle)
        for kind,name,endpoint in [(1,'write_begin','client'),(2,'write_complete','client'),(3,'first_byte','client'),(4,'client_complete','client'),
                                   (8,'parse_complete','server'),(10,'response_enqueued','server'),(15,'output_drained','server')]:
            events.append(normalize_event(connection,sequence,name,points[kind],endpoint,dict(body_verified=True,content_length_verified=True) if kind==4 else None))
        events.append(normalize_event(connection,sequence,'server_first_read',first_read,'server'))
        events.append(normalize_event(connection,sequence,'handler_return',handler,'server'))
        for endpoint,name,start,end in group['intervals']:
            if (endpoint,name,start,end) in prior_idle:continue
            events.append(normalize_event(connection,sequence,'activity',start,endpoint,dict(name=name,end_ns=end)))
        document=dict(metadata=dict(clock='CLOCK_MONOTONIC',unit='ns',overflow=False,writers_stopped=True,endpoints=endpoint_clock,
                                   measurement_start_ns=sample['measurement_start_ns'],measurement_end_ns=sample['measurement_end_ns']),connections=[connection],events=events)
        analyzed=analyze(document)
        request=analyzed['requests'][0]
        # Keep signed endpoint crossovers; all nested scopes are union/intersection, never summed.
        request['connection_index']=connection_index;request['request_sequence']=sequence
        request['client_write_begin_ns']=points[1];request['client_complete_ns']=points[4]
        request['prior_idle_scopes']=len(prior_idle)
        request['prior_idle_scope_evidence']=[dict(endpoint=e,name=n,start_ns=s,end_ns=t,
            reason='fully_paired_scope_ended_before_first_positive_recv') for e,n,s,t in sorted(prior_idle)]
        phase='measurement' if sample['measurement_start_ns']<=points[1] and points[4]<sample['measurement_end_ns'] else 'warmup' if points[4]<sample['measurement_start_ns'] else 'boundary'
        request['phase']=phase;phase_counts[phase]+=1;complete_count+=1
        if points[4]-points[1]>=50_000_000:slow.append(request)
    for row in heapq.merge(*streams):
        timestamp,endpoint,worker,ordinal,index,sequence,kind,value,flags,result=row
        counts[(endpoint,kind)]+=1
        demand((endpoint,index) in by_connection,'record outside frozen filter')
        connection_index=by_connection[(endpoint,index)]
        demand(worker==selected[connection_index][endpoint]['worker'],'wrong record owner')
        if kind==23:continue
        demand(sequence>finalized[connection_index],'late event after finalized request')
        key=(connection_index,sequence)
        group=active.setdefault(key,dict(points={},handlers=[],positive_reads=[],open_calls={},intervals=[],sent_bytes=0,enqueue_bytes=None,body=None))
        if kind==1:
            demand(sequence==origins[connection_index]+1,'client request origin sequence gap/reuse')
            origins[connection_index]=sequence
        if kind in (1,2,3,4,8,9,10,15):
            demand(kind not in group['points'],'duplicate request point')
            group['points'][kind]=timestamp
            if kind==4:group['body']=(value,result,flags)
            if kind==10:group['enqueue_bytes']=value
        if kind==16:group['handlers'].append(timestamp)
        if kind==7 and value>0 and flags==0:group['positive_reads'].append(timestamp)
        if kind in (12,14) and flags==0:group['sent_bytes']+=value
        if kind in PAIRS:
            stack=group['open_calls'].setdefault((endpoint,kind),[])
            demand(kind in (5,17) or not stack,'nested same syscall start')
            stack.append((timestamp,value))
        elif kind in PAIRS.values():
            begin=next(key for key,end in PAIRS.items() if end==kind)
            stack=group['open_calls'].get((endpoint,begin))
            demand(stack,'orphan syscall/session return')
            start,requested=stack.pop()
            if begin in (6,11,13,19,21):
                demand(value<=requested,'IO returned more bytes than requested')
            group['intervals'].append((endpoint,str(begin),start,timestamp))
            if not stack:del group['open_calls'][(endpoint,begin)]
        complete=all(kind in group['points'] for kind in (1,2,3,4,8,9,10,15)) and group['positive_reads'] and any(t>=group['points'][15] for t in group['handlers'])
        if complete:
            finish(key,group)
            finalized[connection_index]=sequence
            del active[key]
    for key,group in sorted(active.items()):finish(key,group,True)
    for connection_index,pair in enumerate(selected):
        end=end_states[('client',pair['client']['index'])]
        demand(origins[connection_index]==(end['reserved'] & 0xffffffff),'captured client origins differ from actual connection lifetime count')
    slow.sort(key=lambda row:row['raw_latency_ns'],reverse=True)
    return dict(schema=2,status='valid',file_catalog=catalog,complete_requests=complete_count,phase_counts=dict(phase_counts),
                idle_groups=idle,idle_reasons=dict(idle_reasons),boundaries=boundaries,event_counts={f'{endpoint}/{kind}':count for (endpoint,kind),count in counts.items()},
                slow_requests=slow,measurement_slow_requests=[row for row in slow if row['phase']=='measurement'],
                limits=['selected fixed connections only; full raw binaries retained','first server read is recv return, not packet-arrival time',
                        'signed cross-endpoint intervals may overlap; syscall scopes are elapsed time, not exclusive CPU',
                        'warmup/cross-window/pending endpoints retained separately; idle/EAGAIN alone does not create a real request'])


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--input',required=True);parser.add_argument('--output',required=True);parser.add_argument('--seconds',type=int,default=15)
    args=parser.parse_args()
    def expired(sig,frame):raise TimeoutError('offline decoder deadline')
    signal.signal(signal.SIGALRM,expired);signal.alarm(args.seconds)
    result=decode(args.input)
    Path(args.output).write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
