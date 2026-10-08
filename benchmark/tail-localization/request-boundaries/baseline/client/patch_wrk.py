#!/usr/bin/env python3
"""固定官方 wrk 4.1.0 的 baseline-v1 接缝；构建槽内调用。"""
import hashlib
import pathlib
import re
import sys

EXPECTED = {
    "wrk.c": "35598eb21cd21d0b4247ae1d7acbf4492f8cbca92f414ddab0fdfada996867a9",
    "wrk.h": "e24694a5785238be8e59e256db08a4e2c2cdefe3fe7f61348282b69c3b823b6d",
    "stats.c": "29ed9ac771ac1309137307d26df8a7e56150e79b4655ba915d0dd3e368d612de",
    "stats.h": "17fbe4a7410effd7de813decd7468362d2e109d27e0fa01210500c46491a9435",
}

def replace_exact(source, old, new):
    if source.count(old) != 1:
        raise ValueError("official source anchor mismatch")
    return source.replace(old, new, 1)

def replace_function(source, name, replacement):
    pattern = re.compile(r"(?m)^(?:static\s+)?(?:int|void\s*\*|void)\s*" + name + r"\([^;]*?\)\s*\{")
    matches = list(pattern.finditer(source))
    if len(matches) != 1:
        raise ValueError(f"function seal mismatch: {name}")
    match = matches[0]
    cursor = match.end()
    depth = 1
    while depth and cursor < len(source):
        depth += (source[cursor] == "{") - (source[cursor] == "}")
        cursor += 1
    if depth:
        raise ValueError(f"unclosed function: {name}")
    return source[:match.start()] + replacement.rstrip() + source[cursor:]

def take_function(source, name):
    pattern = re.compile(r"(?m)^(?:static\s+)?(?:int|void\s*\*|void)\s*" + name + r"\([^;]*?\)\s*\{")
    match = pattern.search(source)
    if match is None:
        raise ValueError(name)
    cursor, depth = match.end(), 1
    while depth:
        depth += (source[cursor] == "{") - (source[cursor] == "}")
        cursor += 1
    return source[match.start():cursor]

def apply_baseline_patch(source_root, module_root):
    source_root = pathlib.Path(source_root)
    module_root = pathlib.Path(module_root)
    for name, expected in EXPECTED.items():
        if expected and hashlib.sha256((source_root / name).read_bytes()).hexdigest() != expected:
            raise ValueError(f"official source drift: {name}")
    header = (source_root / "wrk.h").read_text()
    header = replace_exact(header, '#include "stats.h"', '#include "stats.h"\n#include "MeasurementWindow.h"')
    header = replace_exact(header, '    struct connection *cs;', '''    struct connection *cs;
    CompletionCounters baselineCounters;
    stats *baselineHistograms[5];
    uint64_t baselineClosedCount;
    uint64_t baselineStoppedNs;
    uint64_t baselinePending;
    uint64_t baselinePendingWarm;
    uint64_t baselineCensoredPartial;
    uint64_t baselineCensoredUnsent;
    uint64_t baselineCensoredSent;
    uint64_t baselineHeaderErrors;
    uint64_t baselineBodyErrors;
    uint64_t baselineProtocolErrors;''')
    header = replace_exact(header, '    char buf[RECVBUF];', '''    char buf[RECVBUF];
    RequestState baselineRequest;
    uint64_t baselineReadyNs;
    bool baselineClosed;
    bool baselineStopObserved;
    unsigned int baselineEndReason;''')
    source = (source_root / "wrk.c").read_text()
    callbacks = (module_root / "BaselineCallbacks.inc").read_text()
    for name in ("response_body", "response_complete", "socket_connected", "socket_writeable", "reconnect_socket", "thread_main", "socket_readable", "connect_socket"):
        source = replace_function(source, name, take_function(callbacks, name))
    source = replace_exact(source, 'int main(int argc, char **argv) {', '#include "BaselineRuntime.inc"\n#include "BaselineExport.inc"\nint main(int argc, char **argv) {')
    source = replace_function(source, "main", (module_root / "BaselineMain.inc").read_text())
    (source_root / "wrk.h").write_text(header)
    (source_root / "wrk.c").write_text(source)

if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("expected official src root and own module root")
    apply_baseline_patch(sys.argv[1], sys.argv[2])
