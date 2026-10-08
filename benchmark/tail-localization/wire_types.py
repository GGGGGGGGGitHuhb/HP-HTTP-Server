"""Schema-2 native x86_64 shared startup ABI; no live workload on import."""
import ctypes
import json
import os
from pathlib import Path

class Connection(ctypes.LittleEndianStructure):
    _fields_ = [('ready',ctypes.c_uint32),('worker',ctypes.c_uint32),('pid',ctypes.c_uint32),('tid',ctypes.c_uint32),
                ('starttime',ctypes.c_uint64),('lifetime',ctypes.c_uint64),('localAddress',ctypes.c_uint32),('remoteAddress',ctypes.c_uint32),
                ('localPort',ctypes.c_uint16),('remotePort',ctypes.c_uint16),('fd',ctypes.c_int32),('index',ctypes.c_uint32),('closed',ctypes.c_uint32),('reserved',ctypes.c_uint64)]
class Thread(ctypes.LittleEndianStructure):
    _fields_ = [('ready',ctypes.c_uint32),('pid',ctypes.c_uint32),('tid',ctypes.c_uint32),('worker',ctypes.c_uint32),('starttime',ctypes.c_uint64),('stoppedNs',ctypes.c_uint64)]
class Control(ctypes.LittleEndianStructure):
    _fields_ = [('magic',ctypes.c_char*8),('version',ctypes.c_uint32),('bytes',ctypes.c_uint32),('go',ctypes.c_uint32),('abortRun',ctypes.c_uint32),
                ('connections',ctypes.c_uint32),('detailed',ctypes.c_uint32),('warmupNs',ctypes.c_uint64),('measurementNs',ctypes.c_uint64),('startNs',ctypes.c_uint64),
                ('selectedCount',ctypes.c_uint32),('reserved',ctypes.c_uint32),('selectedServer',ctypes.c_uint32*16),('selectedClient',ctypes.c_uint32*16),
                ('serverThreads',Thread*6),('clientThreads',Thread*3),('serverConnections',Connection*128),('clientConnections',Connection*128)]

class Atomics:
    def __init__(self):
        self.library = ctypes.CDLL('/lib/x86_64-linux-gnu/libatomic.so.1')
        self.load4 = getattr(self.library,'__atomic_load_4')
        self.load4.argtypes = [ctypes.c_void_p,ctypes.c_int]
        self.load4.restype = ctypes.c_uint32
        self.store4 = getattr(self.library,'__atomic_store_4')
        self.store4.argtypes = [ctypes.c_void_p,ctypes.c_uint32,ctypes.c_int]
    def load(self, structure, field):
        return self.load4(ctypes.addressof(structure)+getattr(type(structure),field).offset,2)
    def store(self, structure, field, value):
        self.store4(ctypes.addressof(structure)+getattr(type(structure),field).offset,value,3)

def identity(pid, tid=None):
    target = Path('/proc') / str(pid)
    if tid is not None:
        target = target / 'task' / str(tid)
    line=(target/'stat').read_text()
    fields=line[line.rfind(')')+2:].split()
    status=(target/'status').read_text().splitlines()
    return dict(state=fields[0],pid=pid,tid=tid or pid,starttime=int(fields[19]),ppid=int(fields[1]),session=int(fields[3]),
                nspid=next((x.partition(':')[2].strip() for x in status if x.startswith('NSpid:')),None),
                pid_namespace=os.readlink(target/'ns/pid'),time_namespace=os.readlink(target/'ns/time'),
                boot_id=Path('/proc/sys/kernel/random/boot_id').read_text().strip())

def plain(structure):
    return {name:getattr(structure,name) for name,kind in structure._fields_}

def write_json(path, value):
    path=Path(path)
    temporary=path.with_name(path.name+'.pending-'+str(os.getpid()))
    temporary.write_text(json.dumps(value,indent=2,allow_nan=False)+'\n')
    os.replace(temporary,path)
