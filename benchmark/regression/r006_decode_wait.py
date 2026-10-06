#!/usr/bin/env python3
import argparse,bisect,json,pathlib,struct,subprocess
NAMES={1:'mutex_lock',2:'write',3:'recv',4:'sendfile',5:'fwrite',6:'fflush'}

def main():
    parser=argparse.ArgumentParser();parser.add_argument('root',type=pathlib.Path); args=parser.parse_args()
    summaries=[]
    for directory in sorted(args.root.glob('*-P3-E')):
        data=(directory/'server.wait.bin').read_bytes();slot_size=(3+14)*8+2048*64
        header=struct.unpack_from('<8Q',data)
        assert header[:5]==(0x5230303657414954,1,slot_size,16,2048) and header[5]<=16 and header[7]==1
        process=json.loads((directory/'server.process.json').read_text())
        assert header[6]==process['pid'] and len(data)==64+16*slot_size
        mappings=[]
        for line in (directory/'server.wait.bin.maps').read_text().splitlines():
            parts=line.split()
            if len(parts)>=6 and parts[5].startswith('/'):
                start,end=[int(x,16) for x in parts[0].split('-')]
                mappings.append((start,end,int(parts[2],16),parts[5]))
        symbols={}
        def symbolize(address):
            for start,end,offset,path in mappings:
                if start<=address<end:
                    relative=address-start+offset
                    if path not in symbols:
                        out=subprocess.run(['nm','-n','-C',path],capture_output=True,text=True)
                        entries=[]
                        for line in out.stdout.splitlines():
                            parts=line.split(maxsplit=2)
                            if len(parts)==3:
                                try: entries.append((int(parts[0],16),parts[2]))
                                except ValueError: pass
                        symbols[path]=sorted(entries)
                    entries=symbols[path];index=bisect.bisect_right([x[0] for x in entries],relative)-1
                    return dict(module=path,offset=hex(relative),symbol=entries[index][1] if index>=0 else 'unknown',symbol_delta=relative-entries[index][0] if index>=0 else None)
            return {'address':hex(address),'symbol':'unmapped'}
        events=[];slots=[]
        for index in range(16):
            offset=64+index*slot_size
            header=struct.unpack_from('<17Q',data,offset)
            tid,used,dropped=header[:3]
            assert used<=2048 and dropped==0
            if not tid: continue
            slots.append(dict(tid=tid,calls=dict(zip(NAMES.values(),header[4:10])),max_ms=dict(zip(NAMES.values(),[x/1e6 for x in header[11:17]])),used=used,dropped=dropped))
            for i in range(used):
                tid,kind,start,elapsed,cpu,obj,caller,result=struct.unpack_from('<8Q',data,offset+136+i*64)
                assert kind in NAMES and tid==header[0] and elapsed>=5000000 and cpu<=elapsed+1000000
                events.append(dict(tid=tid,kind=NAMES[kind],start_monotonic=start/1e9,wall_ms=elapsed/1e6,cpu_ms=cpu/1e6,object=hex(obj),caller=symbolize(caller),mutex=symbolize(obj) if kind==1 else None,result=result))
        meta=json.loads((directory/'measurement.histogram.json').read_text())
        sample=json.loads((directory/'sample.json').read_text())
        end=meta['entry_monotonic'];begin=end-sample['measurement']['elapsed_seconds']
        measurement=[event for event in events if begin<=event['start_monotonic']<=end]
        summary=dict(sample=directory.name,slots=slots,all_events=len(events),measurement_events=len(measurement),measurement_window_approximate=[begin,end],events=sorted(measurement,key=lambda x:x['wall_ms'],reverse=True),preload_mapped=any('wait_probe.so' in x[3] for x in mappings))
        (directory/'wait-summary.json').write_text(json.dumps(summary,indent=2))
        summaries.append({k:v for k,v in summary.items() if k!='events'} | {'longest':summary['events'][:12]})
    (args.root/'wait-summary.json').write_text(json.dumps(summaries,indent=2))
    print(json.dumps(summaries,indent=2))
if __name__=='__main__':main()
