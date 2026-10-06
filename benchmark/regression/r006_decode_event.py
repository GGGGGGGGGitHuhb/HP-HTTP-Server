#!/usr/bin/env python3
import argparse,json,pathlib,struct
NAMES={1:'mutex_lock',2:'write',3:'recv',4:'sendfile',5:'fwrite',6:'fflush',7:'read',8:'epoll_wait',9:'between_epoll_calls'}

def main():
    p=argparse.ArgumentParser();p.add_argument('root',type=pathlib.Path);args=p.parse_args();summaries=[]
    for directory in sorted(args.root.glob('*-P3-*')):
        meta=json.loads((directory/'measurement.histogram.json').read_text())
        sample=json.loads((directory/'sample.json').read_text())
        end=meta['entry_monotonic'];begin=end-sample['measurement']['elapsed_seconds']
        for process in ('server','measurement'):
            trace=directory/(process+'.wait.bin');data=trace.read_bytes();header=struct.unpack_from('<8Q',data)
            slot_size=184+2048*64
            assert header[:5]==(0x5230303657414954,1,slot_size,16,2048) and len(data)==64+16*slot_size and header[5]<=16
            slots=[];events=[]
            for index in range(16):
                offset=64+index*slot_size;values=struct.unpack_from('<23Q',data,offset)
                tid,used,dropped=values[:3];assert used<=2048 and dropped==0
                if not tid:continue
                slots.append(dict(tid=tid,used=used,unpaired_epfd=values[3],calls=dict(zip(NAMES.values(),values[4:13])),max_ms=dict(zip(NAMES.values(),[x/1e6 for x in values[14:23]]))))
                for event in range(used):
                    tid,kind,start,elapsed,cpu,obj,caller,result=struct.unpack_from('<8Q',data,offset+184+event*64)
                    assert tid==values[0] and kind in NAMES and elapsed>=5000000
                    if begin<=start/1e9<=end:
                        events.append(dict(tid=tid,kind=NAMES[kind],start_monotonic=start/1e9,end_monotonic=(start+elapsed)/1e9,wall_ms=elapsed/1e6,cpu_ms=cpu/1e6,object=obj,caller=hex(caller),result=result if result<2**63 else result-2**64))
            assert len(slots)==header[5] and len({s['tid'] for s in slots})==len(slots)
            events.sort(key=lambda e:e['wall_ms'],reverse=True)
            record=dict(sample=directory.name,process=process,pid=header[6],measurement_window_approximate=[begin,end],slots=slots,measurement_events=events)
            (directory/(process+'.event-summary.json')).write_text(json.dumps(record,indent=2));summaries.append(record)
    (args.root/'event-summary.json').write_text(json.dumps(summaries,indent=2))
    for record in summaries:
        print(record['sample'],record['process'],'events',len(record['measurement_events']))
        for event in record['measurement_events'][:12]: print(event['tid'],event['kind'],round(event['wall_ms'],3),round(event['cpu_ms'],3),event['result'])
if __name__=='__main__':main()
