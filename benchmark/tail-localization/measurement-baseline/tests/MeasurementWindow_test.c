/* 编译本文件时包含真正补丁后的 wrk.c；只 mock 系统触发，不复制 callback。 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static uint64_t testClockNs;
static int testClockFailure;
static int testClockGettime(clockid_t clock, struct timespec *value) {
    assert(clock == CLOCK_MONOTONIC);
    if (testClockFailure) return -1;
    value->tv_sec = testClockNs / 1000000000;
    value->tv_nsec = testClockNs % 1000000000;
    return 0;
}
static int (*testRealClose)(int) = close;
static int testClose(int fd) { return fd == 0 ? 0 : testRealClose(fd); }
#define clock_gettime testClockGettime
#define close testClose
#define main baselineProgramMain
#include "wrk.c"
#undef main
#undef close
#undef clock_gettime

static unsigned int writeAttempt;
static status testWrite(connection *c, char *bytes, size_t length, size_t *sent) {
    (void)c; (void)bytes;
    writeAttempt++;
    if (writeAttempt == 1) { errno = EAGAIN; return RETRY; }
    *sent = writeAttempt == 2 ? 3 : length;
    return OK;
}

static void resetFailure(void) {
    baselineFailed = 0;
    baselineFirstReason = NULL;
    baselineFirstErrno = 0;
    testClockFailure = 0;
}

static void testClassification(void) {
    MeasurementWindow window = {100, 200, 400};
    assert(classifyCompletion(&window, 100, 199) == kWarmupCompletion);
    assert(classifyCompletion(&window, 100, 200) == kCrossWarmupCompletion);
    assert(classifyCompletion(&window, 200, 399) == kMeasurementCompletion);
    assert(classifyCompletion(&window, 200, 400) == kAfterWindowCompletion);
    assert(classifyCompletion(&window, 400, 401) == kInvalidCompletion);
    assert(!allowRequestWrite(&window, 400));
    uint64_t interval = 0;
    assert(!calculateCorrectionInterval(20000000000, 127, 128, &interval));
    assert(!calculateCorrectionInterval(1000, 256, 128, &interval));
    assert(calculateCorrectionInterval(20000000000, 1113756, 128, &interval));
    assert(interval == 2298);
}

static void testOriginalCorrection(void) {
    stats *raw = stats_alloc(HP_BASELINE_MAX_LATENCY_US);
    stats *corrected = stats_alloc(HP_BASELINE_MAX_LATENCY_US);
    assert(stats_record(raw, 4000));
    assert(correctHistogram(raw, corrected, 1000));
    assert(corrected->count == 3 && corrected->min == 4000);
    assert(corrected->data[2000] == 1 && corrected->data[3000] == 1);
    assert(stats_percentile(corrected, 50) == 0 && stats_percentile(corrected, 99) == 0);
    assert(stats_record(raw, 500));
    assert(correctHistogram(raw, corrected, 1000));
    assert(corrected->count == 4 && stats_percentile(corrected, 50) == 3000);
    assert(stats_percentile(corrected, 99) == 4000);
    raw->count = UINT64_MAX;
    assert(!correctHistogram(raw, corrected, 1000));
    raw->count = 2;
    assert(!stats_record(raw, HP_BASELINE_MAX_LATENCY_US + 1));
    assert(stats_record(raw, 0));
    assert(validateHistogram(raw));
    stats_free(raw);
    stats_free(corrected);
}

static void testActualCallbacks(void) {
    resetFailure();
    baselineWindow = (MeasurementWindow){1000000000, 2000000000, 4000000000};
    baselineGatePublished = 1;
    thread owner = {0};
    connection c = {0};
    owner.connections = 1;
    owner.loop = aeCreateEventLoop(8);
    owner.cs = &c;
    baselineAllocateCounters(&owner);
    c.thread = &owner;
    c.fd = 0;
    c.request = "0123456789";
    c.length = 10;
    c.baselineRequest.lifetime = 1;
    sock.write = testWrite;
    writeAttempt = 0;
    testClockNs = 1990000000;
    socket_writeable(owner.loop, 0, &c, 0);
    assert(c.baselineRequest.requestStarted && c.written == 0);
    assert(c.baselineRequest.firstWriteAttemptNs == 1990000000);
    testClockNs = 2000000000;
    socket_writeable(owner.loop, 0, &c, 0);
    assert(c.written == 3 && c.baselineRequest.firstWriteAttemptNs == 1990000000);
    socket_writeable(owner.loop, 0, &c, 0);
    assert(c.written == 10 && c.baselineRequest.requestFullySent && c.pending == 1);
    unsigned char body[1024];
    for (unsigned int index = 0; index < 1024; index++) body[index] = index % 256;
    http_parser_init(&c.parser, HTTP_RESPONSE);
    c.parser.data = &c;
    parser_settings.on_headers_complete = baselineHeadersComplete;
    parser_settings.on_body = response_body;
    const char *headers = "HTTP/1.1 200 OK\r\nContent-Length: 1024\r\nConnection: keep-alive\r\n\r\n";
    assert(http_parser_execute(&c.parser, &parser_settings, headers, strlen(headers)) == strlen(headers));
    assert(http_parser_execute(&c.parser, &parser_settings, (char *)body, 7) == 7);
    testClockNs = 2010000000;
    /* 测量窗内完成会安排下一写事件，fd0由epoll拒绝；记录本身须恰好一次。 */
    assert(http_parser_execute(&c.parser, &parser_settings, (char *)body + 7, 1017) == 1017);
    assert(owner.baselineCounters.mainCount == 1);
    assert(owner.baselineCounters.completed[kCrossWarmupCompletion] == 1);
    assert(!c.baselineRequest.requestStarted);
    assert(!completeResponse(&baselineWindow, &owner.baselineCounters, &c.baselineRequest, testClockNs));
    resetFailure();
    c.baselineClosed = false;
    owner.baselineClosedCount = 0;
    c.baselineStopObserved = false;
    c.written = 0;
    writeAttempt = 0;
    testClockNs = 3990000000;
    socket_writeable(owner.loop, 0, &c, 0);
    socket_writeable(owner.loop, 0, &c, 0);
    assert(c.baselineRequest.requestStarted);
    uint64_t attempts = writeAttempt;
    testClockNs = 4000000000;
    socket_writeable(owner.loop, 0, &c, 0);
    assert(writeAttempt == attempts);
    assert(c.written == 3 && !c.baselineRequest.requestFullySent);
    assert(markWindowStop(&c.baselineRequest) == kCensoredPartialRequest);
    baselineWindowTick(owner.loop, 0, &owner);
    assert(c.baselineClosed && owner.baselineCensoredPartial == 1);
    resetFailure();
    c.baselineClosed = false;
    c.baselineStopObserved = false;
    owner.baselineClosedCount = 0;
    c.baselineRequest = (RequestState){.lifetime=1, .sequence=1, .firstWriteAttemptNs=1900000000,
                                     .requestStarted=true, .requestFullySent=true};
    c.parser.data = &c;
    testClockNs = 3999999999;
    assert(response_complete(&c.parser) == -1);
    assert(baselineFailed && owner.errors.timeout == 1);
    for (unsigned int index = 0; index < 5; index++) stats_free(owner.baselineHistograms[index]);
    zfree(owner.baselineCounters.slowRecords);
    aeDeleteEventLoop(owner.loop);
}

static void testCounterAndBoundaryFailures(void) {
    resetFailure();
    baselineWindow = (MeasurementWindow){1000000000, 2000000000, 5000000000};
    thread owner = {0};
    connection c = {0};
    owner.loop = aeCreateEventLoop(8);
    owner.connections = 1;
    owner.cs = &c;
    c.thread = &owner;
    c.fd = 0;
    c.parser.data = &c;
    baselineAllocateCounters(&owner);
    RequestState request = {.lifetime=1, .sequence=1, .firstWriteAttemptNs=2000000000,
                            .requestStarted=true, .requestFullySent=true};
    owner.baselineCounters.slowCount = HP_BASELINE_SLOW_CAPACITY;
    assert(!recordCompletion(&baselineWindow, &owner.baselineCounters, &request, 2100000000, 200, 1024));
    assert(owner.baselineCounters.mainCount == 0 && owner.baselineCounters.mainBins[100000] == 0);
    owner.baselineCounters.slowCount = 0;
    owner.baselineCounters.overflow = false;
    owner.baselineCounters.mainBins[100000] = UINT64_MAX;
    assert(!recordCompletion(&baselineWindow, &owner.baselineCounters, &request, 2100000000, 200, 1024));
    assert(owner.baselineCounters.mainCount == 0);
    owner.baselineCounters.mainBins[100000] = 0;
    owner.baselineCounters.overflow = false;
    assert(recordCompletion(&baselineWindow, &owner.baselineCounters, &request, 4000000000, 200, 1024));
    assert(owner.baselineCounters.mainBins[2000000] == 1);
    request.requestStarted = true;
    assert(!recordCompletion(&baselineWindow, &owner.baselineCounters, &request, 4000001000, 200, 1024));
    c.baselineRequest = (RequestState){.lifetime=1, .sequence=2, .firstWriteAttemptNs=4900000000,
                                      .requestStarted=true, .requestFullySent=true};
    c.parser.http_major = 1;
    c.parser.http_minor = 1;
    c.parser.status_code = 500;
    c.parser.content_length = 1024;
    assert(baselineHeadersComplete(&c.parser) == -1 && owner.errors.status == 1);
    resetFailure();
    c.baselineRequest.responseInvalid = false;
    c.parser.status_code = 200;
    assert(baselineHeadersComplete(&c.parser) == 0);
    unsigned char invalid = 1;
    assert(response_body(&c.parser, (char *)&invalid, 1) == -1 && owner.baselineBodyErrors == 1);
    resetFailure();
    c.baselineRequest.responseInvalid = false;
    c.baselineRequest.receivedBodyBytes = 0;
    unsigned char body[1024];
    for (unsigned int index = 0; index < 1024; index++) body[index] = index % 256;
    assert(response_body(&c.parser, (char *)body, 1024) == 0);
    testClockNs = 5000000000;
    uint64_t previousMain = owner.baselineCounters.mainCount;
    assert(response_complete(&c.parser) == 0);
    assert(owner.baselinePending == 1 && c.baselineRequest.pendingAtWindowEnd);
    assert(owner.baselineCounters.completed[kAfterWindowCompletion] == 1);
    assert(owner.baselineCounters.mainCount == previousMain && c.baselineEndReason == 3);
    c.baselineClosed = false;
    c.baselineStopObserved = false;
    owner.baselineClosedCount = 0;
    c.baselineRequest = (RequestState){.lifetime=1, .sequence=3, .firstWriteAttemptNs=4900000000,
                                      .requestStarted=true, .requestFullySent=true};
    testClockNs = 6900000000;
    assert(response_complete(&c.parser) == 0);
    assert(owner.baselineCensoredSent == 1 && c.baselineEndReason == 4 && !baselineFailed);
    for (unsigned int index = 0; index < 5; index++) stats_free(owner.baselineHistograms[index]);
    zfree(owner.baselineCounters.slowRecords);
    aeDeleteEventLoop(owner.loop);
}

int main(void) {
    testClassification();
    testOriginalCorrection();
    testActualCallbacks();
    testCounterAndBoundaryFailures();
    puts("baseline actual C module/callback/correction cases passed");
    return 0;
}
