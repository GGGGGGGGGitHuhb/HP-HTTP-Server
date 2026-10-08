#include "MeasurementWindow.h"
#include <limits.h>
#include <stddef.h>

bool validateWindow(const MeasurementWindow *window) {
    return window && window->warmStartNs < window->measurementStartNs &&
           window->measurementStartNs < window->measurementEndNs;
}

bool allowRequestWrite(const MeasurementWindow *window, uint64_t attemptNs) {
    return validateWindow(window) && attemptNs >= window->warmStartNs &&
           attemptNs < window->measurementEndNs;
}

bool beginRequest(const MeasurementWindow *window, RequestState *request,
                  uint64_t attemptNs) {
    if (!request || !allowRequestWrite(window, attemptNs)) return false;
    /* 短写和 EAGAIN 保留第一次 syscall 尝试时刻。 */
    if (!request->requestStarted) {
        if (request->sequence == UINT64_MAX) return false;
        request->sequence++;
        request->firstWriteAttemptNs = attemptNs;
        request->requestStarted = true;
        request->sentBytes = 0;
        request->requestFullySent = false;
        request->pendingAtWindowEnd = false;
        request->receivedBodyBytes = 0;
        request->responseStatus = 0;
        request->responseHeadersVerified = false;
        request->responseInvalid = false;
    }
    return true;
}

CompletionClass classifyCompletion(const MeasurementWindow *window,
                                   uint64_t startNs, uint64_t completeNs) {
    if (!validateWindow(window) || startNs < window->warmStartNs || startNs >= window->measurementEndNs || completeNs < startNs)
        return kInvalidCompletion;
    if (completeNs >= window->measurementEndNs) return kAfterWindowCompletion;
    if (completeNs < window->measurementStartNs) return kWarmupCompletion;
    if (startNs < window->measurementStartNs) return kCrossWarmupCompletion;
    return kMeasurementCompletion;
}

bool recordCompletion(const MeasurementWindow *window, CompletionCounters *counters,
                      RequestState *request, uint64_t completeNs,
                      uint32_t status, uint64_t bodyBytes) {
    if (!counters || !request || !request->requestStarted || !request->requestFullySent ||
        status != 200 || bodyBytes != 1024) return false;
    CompletionClass completionClass = classifyCompletion(window, request->firstWriteAttemptNs,
                                                         completeNs);
    if (completionClass == kInvalidCompletion) return false;
    uint64_t latencyUs = (completeNs - request->firstWriteAttemptNs) / 1000;
    if (latencyUs > HP_BASELINE_MAX_LATENCY_US) {
        counters->overflow = true;
        return false;
    }
    uint64_t *classBins[] = {counters->warmupBins, counters->crossWarmupBins,
                            counters->measurementStartBins, counters->afterWindowBins};
    bool inMeasurement = completionClass == kCrossWarmupCompletion ||
                         completionClass == kMeasurementCompletion;
    bool slow = inMeasurement && completeNs - request->firstWriteAttemptNs >= 50000000;
    if (!classBins[completionClass] || counters->completed[completionClass] == UINT64_MAX ||
        classBins[completionClass][latencyUs] == UINT64_MAX ||
        (inMeasurement && (!counters->mainBins || counters->mainCount == UINT64_MAX ||
                           counters->mainBins[latencyUs] == UINT64_MAX)) ||
        (completionClass == kMeasurementCompletion && counters->measurementStartCount == UINT64_MAX) ||
        (slow && (!counters->slowRecords || counters->slowCount >= HP_BASELINE_SLOW_CAPACITY))) {
        counters->overflow = true;
        return false;
    }
    /* 全部检查先于 mutation，失败不留下半个样本。 */
    counters->completed[completionClass]++;
    classBins[completionClass][latencyUs]++;
    if (inMeasurement) {
        counters->mainCount++;
        counters->mainBins[latencyUs]++;
    }
    if (completionClass == kMeasurementCompletion) counters->measurementStartCount++;
    if (slow) {
        counters->slowRecords[counters->slowCount++] = (SlowCompletion){
            request->lifetime, request->sequence, request->firstWriteAttemptNs,
            completeNs, bodyBytes, status, completionClass};
    }
    /* 同一 parser completion 不能被重复计数；下一请求另行 begin。 */
    request->requestStarted = false;
    return true;
}

bool calculateCorrectionInterval(uint64_t durationNs, uint64_t completedCount,
                                 uint64_t connectionCount, uint64_t *intervalUs) {
    if (!connectionCount || !intervalUs) return false;
    uint64_t completePerConnection = completedCount / connectionCount;
    if (!completePerConnection) return false;
    *intervalUs = (durationNs / 1000) / completePerConnection;
    return *intervalUs != 0;
}

bool acceptResponseHeaders(RequestState *request, uint32_t status, uint64_t contentLength) {
    if (!request || !request->requestStarted) return false;
    if (request->responseHeadersVerified || status != 200 || contentLength != 1024) {
        request->responseInvalid = true;
        return false;
    }
    request->responseStatus = status;
    request->responseHeadersVerified = true;
    return true;
}

bool acceptResponseBody(RequestState *request, const unsigned char *bytes, uint64_t byteCount) {
    if (!request || !request->requestStarted || request->responseInvalid ||
        !request->responseHeadersVerified || (byteCount && !bytes)) return false;
    if (request->receivedBodyBytes > 1024 || byteCount > 1024 - request->receivedBodyBytes) {
        request->responseInvalid = true;
        return false;
    }
    for (uint64_t offset = 0; offset < byteCount; offset++) {
        if (bytes[offset] != (unsigned char)((request->receivedBodyBytes + offset) % 256)) {
            request->responseInvalid = true;
            return false;
        }
    }
    request->receivedBodyBytes += byteCount;
    return true;
}

bool completeResponse(const MeasurementWindow *window, CompletionCounters *counters,
                      RequestState *request, uint64_t completeNs) {
    if (!request || request->responseInvalid || !request->responseHeadersVerified ||
        request->receivedBodyBytes != 1024) return false;
    return recordCompletion(window, counters, request, completeNs,
                            request->responseStatus, request->receivedBodyBytes);
}

WindowStopAction markWindowStop(RequestState *request) {
    if (!request || !request->requestStarted) return kNoPendingRequest;
    request->pendingAtWindowEnd = true;
    return request->requestFullySent ? kReceivePendingResponse : kCensoredPartialRequest;
}
