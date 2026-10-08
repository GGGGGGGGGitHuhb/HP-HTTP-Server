"""Patch an independent complete Debian wrk source copy; preserve baseline source."""
from pathlib import Path
import argparse
import shutil


def replace(text, old, new, count=1):
    if text.count(old) != count:
        raise ValueError('client seam drift: ' + old[:70])
    return text.replace(old, new)


def prepare(source, destination, tools):
    if destination.exists():
        raise ValueError('diagnostic client source already exists')
    shutil.copytree(source, destination, ignore=shutil.ignore_patterns('obj', 'wrk'))
    for path in [tools / 'common/TailWire.h', tools / 'common/ClockEvidence.h', tools / 'client/ClientObserver.h', tools / 'client/ClientObserver.c']:
        shutil.copyfile(path, destination / 'src' / path.name)
    header = destination / 'src/wrk.h'
    text = header.read_text()
    text = replace(text, '    pthread_t thread;', '    uint32_t observerWorker;\n    pthread_t thread;')
    text = replace(text, '    thread *thread;', '    uint32_t observerIndex, observerSequence;\n    uint64_t observerBodyBytes, observerStartNs;\n    bool observerActive, observerFirstByte, observerBodyValid, observerHeadersValid;\n    thread *thread;')
    header.write_text(text)
    path = destination / 'src/wrk.c'
    text = '#include "ClientObserver.h"\n' + path.read_text()
    text = replace(text, '    stats *latency;', '    stats *rawLatency;\n    stats *latency;')
    text = replace(text, '    statistics.latency  = stats_alloc(cfg.timeout * 1000);', '    statistics.rawLatency = stats_alloc(cfg.timeout * 1000);\n    statistics.latency  = stats_alloc(cfg.timeout * 1000);')
    # Static declarations before the unmodified callback registration.
    text = replace(text, 'static struct http_parser_settings parser_settings = {',
                   'static int observeHeadersComplete(http_parser *parser);\nstatic int observeResponseBody(http_parser *parser, const char *at, size_t length);\nstatic int observerGate(aeEventLoop *loop, long long id, void *data);\n\nstatic struct http_parser_settings parser_settings = {')
    text = replace(text, '    .on_message_complete = response_complete',
                   '    .on_headers_complete = observeHeadersComplete,\n    .on_body = observeResponseBody,\n    .on_message_complete = response_complete')
    text = replace(text, '    for (uint64_t i = 0; i < cfg.threads; i++) {\n        thread *t      = &threads[i];',
                   '    if (cfg.ctx || cfg.delay || cfg.dynamic || cfg.threads != 2 || (cfg.script && (!getenv("HP_S4_TRUSTED_SUMMARY") || strcmp(cfg.script, getenv("HP_S4_TRUSTED_SUMMARY"))))) {\n        fprintf(stderr, "diagnostic client requires fixed plain HTTP without script\\n");\n        return 3;\n    }\n    initializeClientObserver(cfg.connections, cfg.threads);\n    for (uint64_t i = 0; i < cfg.threads; i++) {\n        thread *t      = &threads[i];\n        t->observerWorker = i;')
    text = replace(text, '            cfg.delay    = script_has_delay(t->L);',
                   '            cfg.delay    = script_has_delay(t->L);\n            if (cfg.pipeline != 1 || cfg.dynamic || cfg.delay) abort();')
    text = replace(text, '    uint64_t start    = time_us();',
                   '    while (!__atomic_load_n(&clientControl()->clientThreads[0].ready, __ATOMIC_ACQUIRE) || !__atomic_load_n(&clientControl()->clientThreads[1].ready, __ATOMIC_ACQUIRE)) usleep(1000);\n    const char *markerFd = getenv("HP_S4_MARKER_FD");\n    if (markerFd && close(atoi(markerFd))) abort();\n    unsetenv("HP_S4_MARKER_FD");\n    while (!__atomic_load_n(&clientControl()->go, __ATOMIC_ACQUIRE) &&\n           !__atomic_load_n(&clientControl()->abortRun, __ATOMIC_ACQUIRE)) usleep(1000);\n    uint64_t measurementStartNs = clientControl()->startNs + clientControl()->warmupNs;\n    uint64_t measurementEndNs = measurementStartNs + clientControl()->measurementNs;\n    if (clientControl()->measurementNs != cfg.duration * 1000000000ULL) abort();\n    uint64_t goObservedNs = clientMonotonicNs();\n    if (!__atomic_load_n(&clientControl()->go, __ATOMIC_ACQUIRE) || goObservedNs >= measurementStartNs) abort();\n    uint64_t start = time_us() + (measurementStartNs - goObservedNs) / 1000;')
    text = replace(text, '    sleep(cfg.duration);',
                   '    while (clientMonotonicNs() < measurementEndNs &&\n           !__atomic_load_n(&clientControl()->abortRun, __ATOMIC_ACQUIRE)) usleep(1000);')
    text = replace(text, '    if (complete / cfg.connections > 0) {',
                   '    printf("S4 raw latency us: count=%llu p50=%llu p99=%llu max=%llu\\n",\n           (unsigned long long)statistics.rawLatency->count,\n           (unsigned long long)stats_percentile(statistics.rawLatency, 50),\n           (unsigned long long)stats_percentile(statistics.rawLatency, 99),\n           (unsigned long long)statistics.rawLatency->max);\n    if (complete / cfg.connections > 0) {')
    text = replace(text, '    return 0;\n}\n\nvoid *thread_main',
                   '    int observerInvalid = __atomic_load_n(&clientControl()->abortRun, __ATOMIC_ACQUIRE);\n    exportClientObserver();\n    return observerInvalid ? 3 : 0;\n}\n\nvoid *thread_main')
    text = replace(text, '    thread *thread = arg;\n', '    thread *thread = arg;\n    registerClientThread(thread->observerWorker);\n')
    text = replace(text, '        c->thread = thread;',
                   '        c->thread = thread;\n        c->observerIndex = thread->observerWorker * thread->connections + i;')
    text = replace(text, '    aeCreateTimeEvent(loop, RECORD_INTERVAL_MS, record_rate, thread, NULL);',
                   '    aeCreateTimeEvent(loop, RECORD_INTERVAL_MS, record_rate, thread, NULL);\n    aeCreateTimeEvent(loop, 1, observerGate, thread, NULL);')
    text = replace(text, '    aeDeleteEventLoop(loop);',
                   '    for (uint64_t i = 0; i < thread->connections; ++i) {\n        connection *item = &thread->cs[i];\n        if (item->fd >= 0) { sock.close(item); close(item->fd); }\n        clientControl()->clientConnections[item->observerIndex].reserved = item->observerSequence | ((uint64_t)item->observerActive << 32);\n        __atomic_store_n(&clientControl()->clientConnections[item->observerIndex].closed, 1U, __ATOMIC_RELEASE);\n    }\n    stopClientThread(thread->observerWorker);\n    aeDeleteEventLoop(loop);')
    text = replace(text, 'static int reconnect_socket(thread *thread, connection *c) {',
                   'static int reconnect_socket(thread *thread, connection *c) {\n    // No diagnostic reconnect: preserve lifetime and stop on the first transport failure.\n    abortClientObserver();\n    stop = 1;\n    return -1;\n#if 0')
    text = replace(text, '    return connect_socket(thread, c);\n}', '    return connect_socket(thread, c);\n#endif\n}')
    # Warmup does not write measurement histograms. Rate algorithm remains the original 100ms bucket.
    text = replace(text, '    if (thread->requests > 0) {',
                   '    if (thread->requests > 0 && clientMonotonicNs() >= clientControl()->startNs + clientControl()->warmupNs) {')
    text = replace(text, '        uint64_t elapsed_ms = (time_us() - thread->start) / 1000;', '        uint64_t rateNowUs = time_us();\n        if (rateNowUs <= thread->start || (rateNowUs - thread->start) / 1000 == 0) { abortClientObserver(); stop = 1; return AE_NOMORE; }\n        uint64_t elapsed_ms = (rateNowUs - thread->start) / 1000;')
    text = replace(text, '    thread->complete++;\n    thread->requests++;',
                   '    if (!c->observerActive || !c->observerHeadersValid || !c->observerBodyValid || c->observerBodyBytes != 1024) {\n        abortClientObserver(); stop = 1; return 1;\n    }\n    appendClientEvent(thread->observerWorker, c->observerIndex, c->observerSequence, 4, c->observerBodyBytes, status, 1);\n    c->observerActive = false;\n    uint64_t completionNs = clientMonotonicNs();\n    uint64_t measurementBeginNs = clientControl()->startNs + clientControl()->warmupNs;\n    bool measurement = completionNs >= measurementBeginNs && completionNs < measurementBeginNs + clientControl()->measurementNs;\n    if (measurement) { thread->complete++; thread->requests++;\n        if (!stats_record(statistics.rawLatency, (clientMonotonicNs() - c->observerStartNs) / 1000)) { abortClientObserver(); stop = 1; }\n    }')
    text = replace(text, '        if (!stats_record(statistics.latency, now - c->start)) {',
                   '        if (measurement && !stats_record(statistics.latency, now - c->start)) {')
    text = replace(text, '    aeCreateFileEvent(c->thread->loop, fd, AE_WRITABLE, socket_writeable, c);\n\n    return;',
                   '    registerClientConnection(c->observerIndex, c->thread->observerWorker, fd);\n    // Prewarmup connection freeze: observerGate adds WRITE only after controller GO.\n    aeDeleteFileEvent(loop, fd, AE_WRITABLE);\n\n    return;')
    text = replace(text, '    if (!c->written) {',
                   '    if (!c->observerActive) {\n        c->observerActive = true;\n        c->observerStartNs = clientMonotonicNs();\n        c->observerFirstByte = false;\n        c->observerBodyBytes = 0;\n        c->observerBodyValid = true;\n        c->observerHeadersValid = false;\n        ++c->observerSequence;\n        appendClientEvent(thread->observerWorker, c->observerIndex, c->observerSequence, 1, c->length, 0, 0);\n    }\n    if (!c->written) {')
    text = replace(text, '    size_t n;\n\n    switch (sock.write(c, buf, len, &n)) {',
                   '    size_t n = 0;\n    appendClientEvent(thread->observerWorker, c->observerIndex, c->observerSequence, 19, len, 0, 0);\n    status writeStatus = sock.write(c, buf, len, &n);\n    appendClientEvent(thread->observerWorker, c->observerIndex, c->observerSequence, 20, n, writeStatus == OK ? 0 : errno, writeStatus);\n    switch (writeStatus) {')
    text = replace(text, '    if (c->written == c->length) {',
                   '    if (c->written == c->length) {\n        appendClientEvent(thread->observerWorker, c->observerIndex, c->observerSequence, 2, c->length, 0, 0);')
    text = replace(text, '    size_t n;\n\n    do {\n        switch (sock.read(c, &n)) {',
                   '    size_t n = 0;\n\n    do {\n        appendClientEvent(c->thread->observerWorker, c->observerIndex, c->observerSequence, 21, RECVBUF, 0, 0);\n        n = 0;\n        status readStatus = sock.read(c, &n);\n        appendClientEvent(c->thread->observerWorker, c->observerIndex, c->observerSequence, 22, n, readStatus == OK ? 0 : errno, readStatus);\n        switch (readStatus) {')
    text = replace(text, '        if (http_parser_execute(&c->parser, &parser_settings, c->buf, n) != n) goto error;',
                   '        if (n && !c->observerFirstByte) {\n            c->observerFirstByte = true;\n            appendClientEvent(c->thread->observerWorker, c->observerIndex, c->observerSequence, 3, n, 0, 0);\n        }\n        if (http_parser_execute(&c->parser, &parser_settings, c->buf, n) != n) goto error;')
    text = replace(text, '        c->thread->bytes += n;',
                   '        uint64_t readCompleteNs = clientMonotonicNs();\n        uint64_t measurementBeginNs = clientControl()->startNs + clientControl()->warmupNs;\n        if (readCompleteNs >= measurementBeginNs && readCompleteNs < measurementBeginNs + clientControl()->measurementNs) c->thread->bytes += n;')
    # Helper hooks preserve the third-party callback ABI; no Lua callback substitutions.
    text += '''\nstatic int observeHeadersComplete(http_parser *parser) {
    connection *c = parser->data;
    c->observerHeadersValid = parser->status_code == 200 && parser->content_length == 1024 && !(parser->flags & F_CHUNKED);
    if (!c->observerHeadersValid) { abortClientObserver(); stop = 1; return 1; }
    return 0;
}

static int observeResponseBody(http_parser *parser, const char *at, size_t length) {
    connection *c = parser->data;
    for (size_t index = 0; index < length; ++index)
        if ((unsigned char)at[index] != (unsigned char)((c->observerBodyBytes + index) & 255)) c->observerBodyValid = false;
    c->observerBodyBytes += length;
    if (c->observerBodyBytes > 1024 || !c->observerBodyValid) { abortClientObserver(); stop = 1; return 1; }
    return 0;
}

static int observerGate(aeEventLoop *loop, long long id, void *data) {
    thread *thread = data;
    HpS4Control *control = clientControl();
    if (__atomic_load_n(&control->abortRun, __ATOMIC_ACQUIRE) || stop) { aeStop(loop); return AE_NOMORE; }
    if (!__atomic_load_n(&control->go, __ATOMIC_ACQUIRE)) return 1;
    if (!thread->start) abort();
    if (thread->start != UINT64_MAX) {
        for (uint64_t index = 0; index < thread->connections; ++index) {
            connection *c = &thread->cs[index];
            if (!__atomic_load_n(&control->clientConnections[c->observerIndex].ready, __ATOMIC_ACQUIRE)) abort();
            aeCreateFileEvent(loop, c->fd, AE_WRITABLE, socket_writeable, c);
        }
        thread->start = UINT64_MAX;
    }
    if (clientMonotonicNs() >= control->startNs + control->warmupNs + control->measurementNs) {
        aeStop(loop); return AE_NOMORE;
    }
    return 1;
}
'''
    # Gate state must not reuse the original rate timestamp (record_rate updates it).
    text = text.replace('if (!thread->start) abort();\n    if (thread->start != UINT64_MAX)', 'if (!thread->observerStarted)')
    text = text.replace('thread->start = UINT64_MAX;', 'thread->observerStarted = true;\n        uint64_t gateNs = clientMonotonicNs();\n        if (gateNs >= control->startNs + control->warmupNs) abort();\n        thread->start = time_us() + (control->startNs + control->warmupNs - gateNs) / 1000;')
    path.write_text(text)
    header.write_text(header.read_text().replace('    uint32_t observerWorker;', '    uint32_t observerWorker;\n    bool observerStarted;'))
    path = destination / 'Makefile'
    text = path.read_text()
    text = replace(text, 'SRC  :=', 'SRC  :=') if False else text
    # ClientObserver lives in src; the upstream wildcard discovers it without changing algorithms.
    text = replace(text, 'SRC  := wrk.c', 'SRC  := ClientObserver.c wrk.c')
    path.write_text(text)

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--destination', type=Path, required=True)
    args = parser.parse_args()
    prepare(args.source.resolve(), args.destination.resolve(), Path(__file__).resolve().parents[1])
