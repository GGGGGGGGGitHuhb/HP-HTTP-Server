static int response_body(http_parser *parser, const char *at, size_t len) {
    connection *c = parser->data;
    if (!acceptResponseBody(&c->baselineRequest, (const unsigned char *)at, len)) {
        c->thread->baselineBodyErrors++;
        baselineFail(c->thread, "response payload");
        return -1;
    }
    return 0;
}

static int response_complete(http_parser *parser) {
    connection *c = parser->data;
    thread *owner = c->thread;
    uint64_t completeNs = baselineClockNs();
    if (completeNs >= baselineWindow.measurementEndNs && !c->baselineStopObserved) {
        c->baselineStopObserved = true;
        WindowStopAction action = markWindowStop(&c->baselineRequest);
        if (action != kNoPendingRequest) {
            owner->baselinePending++;
            if (c->baselineRequest.firstWriteAttemptNs < baselineWindow.measurementStartNs)
                owner->baselinePendingWarm++;
        }
    }
    if (completeNs - c->baselineRequest.firstWriteAttemptNs >= 2000000000) {
        if (c->baselineRequest.firstWriteAttemptNs + 2000000000 < baselineWindow.measurementEndNs) {
            owner->errors.timeout++;
            baselineFail(owner, "late callback for measurement deadline");
            return -1;
        }
        owner->baselineCensoredSent++;
        c->baselineEndReason = 4;
        baselineCloseConnection(c);
        return 0;
    }
    if (!completeResponse(&baselineWindow, &owner->baselineCounters,
                          &c->baselineRequest, completeNs)) {
        owner->baselineProtocolErrors++;
        baselineFail(owner, "response complete");
        return -1;
    }
    owner->complete++;
    c->pending = 0;
    if (!http_should_keep_alive(parser)) {
        baselineFail(owner, "unexpected keepalive close");
        return -1;
    }
    http_parser_init(parser, HTTP_RESPONSE);
    if (completeNs >= baselineWindow.measurementEndNs) {
        c->baselineEndReason = 3;
        baselineCloseConnection(c);
    } else {
        c->written = 0;
        if (aeCreateFileEvent(owner->loop, c->fd, AE_WRITABLE, socket_writeable, c) != AE_OK)
            baselineFail(owner, "next request write");
    }
    return 0;
}

static void socket_connected(aeEventLoop *loop, int fd, void *data, int mask) {
    (void)mask;
    connection *c = data;
    int connectError = 0;
    socklen_t errorSize = sizeof(connectError);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &connectError, &errorSize) || connectError) {
        if (connectError) errno = connectError;
        c->thread->errors.connect++;
        baselineFail(c->thread, "worker connect");
        return;
    }
    http_parser_init(&c->parser, HTTP_RESPONSE);
    c->parser.data = c;
    c->written = 0;
    aeDeleteFileEvent(loop, fd, AE_WRITABLE | AE_READABLE);
    if (aeCreateFileEvent(loop, fd, AE_READABLE, socket_readable, c) != AE_OK) {
        baselineFail(c->thread, "worker readable");
        return;
    }
    c->baselineReadyNs = baselineClockNs();
    __atomic_fetch_add(&baselineReadyCount, 1, __ATOMIC_RELEASE);
}

static void socket_writeable(aeEventLoop *loop, int fd, void *data, int mask) {
    (void)mask;
    connection *c = data;
    uint64_t attemptNs = baselineClockNs();
    if (!__atomic_load_n(&baselineGatePublished, __ATOMIC_ACQUIRE) ||
        !allowRequestWrite(&baselineWindow, attemptNs)) {
        aeDeleteFileEvent(loop, fd, AE_WRITABLE);
        return;
    }
    if (!beginRequest(&baselineWindow, &c->baselineRequest, attemptNs)) {
        baselineFail(c->thread, "request sequence overflow");
        return;
    }
    size_t sent = 0;
    switch (sock.write(c, c->request + c->written, c->length - c->written, &sent)) {
        case OK: break;
        case RETRY: return;
        case ERROR:
            c->thread->errors.write++;
            baselineFail(c->thread, "request write");
            return;
    }
    if (sent > c->length - c->written) {
        baselineFail(c->thread, "write byte count");
        return;
    }
    c->written += sent;
    c->baselineRequest.sentBytes += sent;
    if (c->written == c->length) {
        c->baselineRequest.requestFullySent = true;
        c->pending = 1;
        aeDeleteFileEvent(loop, fd, AE_WRITABLE);
    }
}

static int reconnect_socket(thread *owner, connection *c) {
    (void)c;
    baselineFail(owner, "reconnect forbidden");
    return -1;
}

void *thread_main(void *arg) {
    thread *owner = arg;
    static char request[] = "GET /payload-1024.bin HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: keep-alive\r\n\r\n";
    baselineAllocateCounters(owner);
    owner->cs = zcalloc(owner->connections * sizeof(connection));
    for (uint64_t index = 0; index < owner->connections; index++) {
        connection *c = &owner->cs[index];
        c->fd = -1;
        c->thread = owner;
        c->request = request;
        c->length = sizeof(request) - 1;
        c->baselineRequest.lifetime = index + 1;
        c->baselineRequest.sequence = 0;
        if (connect_socket(owner, c) < 0) baselineFail(owner, "initial connect");
    }
    aeCreateTimeEvent(owner->loop, 1, baselineWindowTick, owner, NULL);
    aeMain(owner->loop);
    for (uint64_t index = 0; index < owner->connections; index++) {
        connection *c = &owner->cs[index];
        if (c->fd >= 0 && !c->baselineClosed) baselineCloseConnection(c);
    }
    owner->baselineStoppedNs = baselineClockNs();
    if (!baselineFinalizeCounters(owner)) baselineFail(owner, "counter finalization");
    aeDeleteEventLoop(owner->loop);
    return NULL;
}

static void socket_readable(aeEventLoop *loop, int fd, void *data, int mask) {
    (void)loop; (void)fd; (void)mask;
    connection *c = data;
    size_t received = 0;
    if (c->baselineClosed) return;
    do {
        switch (sock.read(c, &received)) {
            case OK: break;
            case RETRY: return;
            case ERROR:
                c->thread->errors.read++;
                baselineFail(c->thread, "response read");
                return;
        }
        if (!received || http_parser_execute(&c->parser, &parser_settings, c->buf, received) != received) {
            c->thread->baselineProtocolErrors++;
            c->thread->errors.read++;
            baselineFail(c->thread, "response parser/EOF");
            return;
        }
        c->thread->bytes += received;
    } while (!c->baselineClosed && received == RECVBUF && sock.readable(c) > 0);
}

static int connect_socket(thread *owner, connection *c) {
    struct addrinfo *address = owner->addr;
    int descriptor = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (descriptor < 0) {
        owner->errors.connect++;
        return -1;
    }
    int flags = fcntl(descriptor, F_GETFL, 0);
    if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0) goto failed;
    int enabled = 1;
    if (setsockopt(descriptor, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled))) goto failed;
    if (connect(descriptor, address->ai_addr, address->ai_addrlen) < 0 && errno != EINPROGRESS) goto failed;
    if (aeCreateFileEvent(owner->loop, descriptor, AE_READABLE | AE_WRITABLE, socket_connected, c) != AE_OK) goto failed;
    c->parser.data = c;
    c->fd = descriptor;
    return descriptor;
failed: {
    int failure = errno;
    owner->errors.connect++;
    close(descriptor);
    errno = failure;
    return -1;
}
}
