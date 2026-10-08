/* 直接执行诊断wrk的实际静态回调；全部transport/epoll调用以纯mock替换。 */
#define main diagnosticWrkMain
#include HP_S4_CLIENT_SOURCE
#undef main
#include <assert.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

static int addedWrites;
static int deletedWrites;
static int writeStep;
static const size_t kWriteBytes = 64;

int __wrap_aeCreateFileEvent(aeEventLoop *loop, int fd, int mask, aeFileProc *procedure, void *data) {
    if (mask & AE_WRITABLE) ++addedWrites;
    return AE_OK;
}

void __wrap_aeDeleteFileEvent(aeEventLoop *loop, int fd, int mask) {
    if (mask & AE_WRITABLE) ++deletedWrites;
}

static status simulateShortWrite(connection *item, char *bytes, size_t length, size_t *written) {
    ++writeStep;
    if (writeStep == 1) { *written = 10; return OK; }
    if (writeStep == 2) { *written = 0; errno = EAGAIN; return RETRY; }
    *written = length;
    return OK;
}

static void prepareResponse(connection *item) {
    http_parser_init(&item->parser, HTTP_RESPONSE);
    item->parser.data = item;
    item->parser.http_major = 1;
    item->parser.http_minor = 1;
    item->parser.status_code = 200;
    item->parser.content_length = 1024;
    item->observerHeadersValid = true;
    item->observerBodyValid = true;
    item->observerBodyBytes = 1024;
    item->pending = 1;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    char path[4096];
    snprintf(path, sizeof(path), "%s/control.bin", argv[1]);
    int descriptor = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    assert(descriptor >= 0);
    assert(ftruncate(descriptor, sizeof(HpS4Control)) == 0);
    HpS4Control *shared = mmap(NULL, sizeof(HpS4Control), PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
    assert(shared != MAP_FAILED);
    memcpy(shared->magic, "S4CTRL03", 8);
    shared->version = 3; shared->bytes = sizeof(HpS4Control);
    shared->connections = 2; shared->requestsPerConnection = 16;
    shared->phase = HP_S4_RUNNING; shared->go = 1;
    assert(setenv("HP_S4_CONTROL", path, 1) == 0);
    assert(setenv("HP_S4_OUTPUT", argv[1], 1) == 0);
    initializeClientObserver(2, 2);
    cfg.pipeline = 1;
    thread owner = {0};
    char request[64] = {0};
    connection item = {0};
    item.thread = &owner; item.request = request; item.length = kWriteBytes;
    item.fd = 7;
    sock.write = simulateShortWrite;
    socket_writeable(NULL, item.fd, &item, 0);
    assert(item.written == 10 && item.observerSequence == 1 && item.observerActive);
    socket_writeable(NULL, item.fd, &item, 0);
    assert(item.written == 10 && item.observerSequence == 1 && item.observerActive);
    socket_writeable(NULL, item.fd, &item, 0);
    assert(item.written == 0 && item.observerSequence == 1 && deletedWrites == 1);
    prepareResponse(&item);
    assert(response_complete(&item.parser) == 0);
    assert(addedWrites == 1 && shared->completedConnections == 0);
    for (uint32_t sequence = 2; sequence <= 16; ++sequence) {
        socket_writeable(NULL, item.fd, &item, 0);
        assert(item.observerSequence == sequence);
        prepareResponse(&item);
        assert(response_complete(&item.parser) == 0);
    }
    assert(addedWrites == 15 && shared->completedConnections == 1);
    assert(!item.observerActive && item.observerSequence == 16);
    /* 未满足第二条连接不能停止或以总请求数凑数。 */
    assert(shared->completedConnections < shared->connections);
    shared->go = 0; shared->phase = HP_S4_REGISTERING;
    socket_writeable(NULL, item.fd, &item, 0);
    assert(shared->firstFailure.published == 2 && shared->firstFailure.code == HP_S4_REQUEST_BEFORE_GO);
    assert(item.observerSequence == 16);
    assert(observeHeadersComplete(&item.parser) == 1);
    assert(shared->firstFailure.code == HP_S4_REQUEST_BEFORE_GO);
    close(descriptor);
    munmap(shared, sizeof(HpS4Control));
    return 0;
}
