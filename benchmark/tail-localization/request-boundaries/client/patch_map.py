"""只修改建连cold接缝与停止sidecar；原baseline模块字节不改。"""
from pathlib import Path
import sys
sys.dont_write_bytecode = True


def replace_exact(text, before, after):
    if text.count(before) != 1:
        raise ValueError('baseline map anchor mismatch: ' + before)
    return text.replace(before, after, 1)


def apply_map_patch(source_root, module_root):
    root, module = Path(source_root), Path(module_root)
    header = (root / 'wrk.h').read_text()
    header = replace_exact(header, '    uint64_t baselineReadyNs;', '''    uint32_t boundaryClientAddress;
    uint32_t boundaryServerAddress;
    uint16_t boundaryClientPort;
    uint16_t boundaryServerPort;
    int boundaryConnectedFd;
    uint64_t baselineReadyNs;''')
    source = (root / 'wrk.c').read_text()
    source = replace_exact(source, '#include "BaselineExport.inc"', '#include "BaselineExport.inc"\n#include "ClientMap.inc"')
    source = replace_exact(source, '    c->baselineReadyNs = baselineClockNs();', '''    if (!boundaryCaptureConnection(c)) {
        baselineFail(c->thread, "map connection tuple");
        return;
    }
    c->baselineReadyNs = baselineClockNs();''')
    source = replace_exact(source, '    thread *owners = zcalloc(2 * sizeof(thread));', '    if (!boundaryInitializeIdentity()) return 1;\n    thread *owners = zcalloc(2 * sizeof(thread));')
    source = replace_exact(source, 'baselineExportResult(owners)', '(baselineExportResult(owners) && boundaryExportMap(owners))')
    (root / 'wrk.h').write_text(header)
    (root / 'wrk.c').write_text(source)
    (root / 'ClientMap.inc').write_bytes((module / 'ClientMap.inc').read_bytes())

if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('expected source_root module_root')
    apply_map_patch(sys.argv[1], sys.argv[2])
