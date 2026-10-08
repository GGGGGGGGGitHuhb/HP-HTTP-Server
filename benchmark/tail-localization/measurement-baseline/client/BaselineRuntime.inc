/* 仅固定 ordinary HTTP；这些函数由官方 wrk 实际 socket/parser callback 调用。 */
#include "MeasurementWindow.h"
#include "HistogramExport.h"
#include <time.h>

static MeasurementWindow baselineWindow;
static unsigned int baselineReadyCount;
static unsigned int baselineGatePublished;
static unsigned int baselineFailed;
static const char *baselineFirstReason;
static int baselineFirstErrno;

static uint64_t baselineClockNs(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value)) {
        if (!__atomic_exchange_n(&baselineFailed, 1, __ATOMIC_ACQ_REL)) {
            baselineFirstReason = "CLOCK_MONOTONIC";
            baselineFirstErrno = errno;
        }
        return 0;
    }
    return (uint64_t)value.tv_sec * 1000000000 + (uint64_t)value.tv_nsec;
}

static void baselineFail(thread *owner, const char *reason) {
    if (!__atomic_exchange_n(&baselineFailed, 1, __ATOMIC_ACQ_REL)) {
        baselineFirstReason = reason;
        baselineFirstErrno = errno;
        fprintf(stderr, "baseline first failure: %s errno_snapshot_non_authoritative=%d\n", reason, errno);
    }
    aeStop(owner->loop);
}

static void baselineCloseConnection(connection *c) {
    if (c->baselineClosed) return;
    aeDeleteFileEvent(c->thread->loop, c->fd, AE_WRITABLE | AE_READABLE);
    if (close(c->fd)) baselineFail(c->thread, "close connection");
    c->baselineClosed = true;
    c->thread->baselineClosedCount++;
}

static int baselineHeadersComplete(http_parser *parser) {
    connection *c = parser->data;
    if (!acceptResponseHeaders(&c->baselineRequest, parser->status_code, parser->content_length)) {
        c->thread->baselineHeaderErrors++;
        if (parser->status_code != 200) c->thread->errors.status++;
        baselineFail(c->thread, "response headers");
        return -1;
    }
    return 0;
}

static int baselineWindowTick(aeEventLoop *loop, long long id, void *data) {
    (void)id;
    thread *owner = data;
    uint64_t nowNs = baselineClockNs();
    if (__atomic_load_n(&baselineFailed, __ATOMIC_ACQUIRE)) {
        aeStop(loop);
        return AE_NOMORE;
    }
    if (!__atomic_load_n(&baselineGatePublished, __ATOMIC_ACQUIRE)) return 1;
    for (uint64_t index = 0; index < owner->connections; index++) {
        connection *c = &owner->cs[index];
        if (c->baselineClosed) continue;
        if (nowNs >= baselineWindow.measurementEndNs) {
            aeDeleteFileEvent(loop, c->fd, AE_WRITABLE);
            if (!c->baselineStopObserved) {
                c->baselineStopObserved = true;
                WindowStopAction action = markWindowStop(&c->baselineRequest);
                if (action != kNoPendingRequest) {
                    owner->baselinePending++;
                    if (c->baselineRequest.firstWriteAttemptNs < baselineWindow.measurementStartNs)
                        owner->baselinePendingWarm++;
                }
                if (action == kCensoredPartialRequest) {
                    if (c->baselineRequest.sentBytes == 0) owner->baselineCensoredUnsent++;
                    else owner->baselineCensoredPartial++;
                    c->baselineEndReason = 1;
                    baselineCloseConnection(c);
                    continue;
                }
                if (action == kNoPendingRequest) {
                    c->baselineEndReason = 2;
                    baselineCloseConnection(c);
                    continue;
                }
            }
            if (nowNs - c->baselineRequest.firstWriteAttemptNs >= 2000000000) {
                if (c->baselineRequest.firstWriteAttemptNs + 2000000000 <
                    baselineWindow.measurementEndNs) {
                    owner->errors.timeout++;
                    baselineFail(owner, "request deadline inside measurement");
                    continue;
                }
                owner->baselineCensoredSent++;
                c->baselineEndReason = 4;
                baselineCloseConnection(c);
            }
        } else if (c->baselineRequest.requestStarted &&
                   nowNs - c->baselineRequest.firstWriteAttemptNs >= 2000000000) {
            owner->errors.timeout++;
            baselineFail(owner, "request timeout before T1");
        } else if (nowNs >= baselineWindow.warmStartNs && !c->baselineRequest.requestStarted) {
            if (aeCreateFileEvent(loop, c->fd, AE_WRITABLE, socket_writeable, c) != AE_OK)
                baselineFail(owner, "schedule request write");
        }
    }
    if (owner->baselineClosedCount == owner->connections) {
        owner->baselineStoppedNs = nowNs;
        aeStop(loop);
        return AE_NOMORE;
    }
    return 1;
}

static void baselineAllocateCounters(thread *owner) {
    for (unsigned int index = 0; index < 5; index++)
        owner->baselineHistograms[index] = stats_alloc(HP_BASELINE_MAX_LATENCY_US);
    owner->baselineCounters.mainBins = owner->baselineHistograms[0]->data;
    owner->baselineCounters.measurementStartBins = owner->baselineHistograms[1]->data;
    owner->baselineCounters.crossWarmupBins = owner->baselineHistograms[2]->data;
    owner->baselineCounters.warmupBins = owner->baselineHistograms[3]->data;
    owner->baselineCounters.afterWindowBins = owner->baselineHistograms[4]->data;
    owner->baselineCounters.slowRecords = zcalloc(HP_BASELINE_SLOW_CAPACITY * sizeof(SlowCompletion));
}

static bool baselineFinalizeCounters(thread *owner) {
    uint64_t counts[] = {owner->baselineCounters.mainCount,
                        owner->baselineCounters.measurementStartCount,
                        owner->baselineCounters.completed[kCrossWarmupCompletion],
                        owner->baselineCounters.completed[kWarmupCompletion],
                        owner->baselineCounters.completed[kAfterWindowCompletion]};
    for (unsigned int index = 0; index < 5; index++) {
        stats *histogram = owner->baselineHistograms[index];
        histogram->count = counts[index];
        histogram->min = UINT64_MAX;
        histogram->max = 0;
        for (uint64_t bin = 0; bin < histogram->limit; bin++) {
            if (!histogram->data[bin]) continue;
            if (histogram->min == UINT64_MAX) histogram->min = bin;
            histogram->max = bin;
        }
        if (!validateHistogram(histogram)) return false;
    }
    return !owner->baselineCounters.overflow;
}
