#ifndef HP_BASELINE_MEASUREMENT_WINDOW_H_
#define HP_BASELINE_MEASUREMENT_WINDOW_H_
#include <stdbool.h>
#include <stdint.h>

#define HP_BASELINE_MAX_LATENCY_US 2000000U
#define HP_BASELINE_SLOW_CAPACITY 16384U

typedef enum {
    kWarmupCompletion,
    kCrossWarmupCompletion,
    kMeasurementCompletion,
    kAfterWindowCompletion,
    kInvalidCompletion
} CompletionClass;

typedef struct {
    uint64_t warmStartNs;
    uint64_t measurementStartNs;
    uint64_t measurementEndNs;
} MeasurementWindow;

typedef struct {
    uint64_t lifetime;
    uint64_t sequence;
    uint64_t firstWriteAttemptNs;
    uint64_t sentBytes;
    bool requestStarted;
    bool requestFullySent;
    bool pendingAtWindowEnd;
    uint64_t receivedBodyBytes;
    uint32_t responseStatus;
    bool responseHeadersVerified;
    bool responseInvalid;
} RequestState;

typedef enum {
    kNoPendingRequest,
    kReceivePendingResponse,
    kCensoredPartialRequest
} WindowStopAction;

typedef struct {
    uint64_t lifetime;
    uint64_t sequence;
    uint64_t startNs;
    uint64_t completeNs;
    uint64_t bodyBytes;
    uint32_t status;
    uint32_t completionClass;
} SlowCompletion;

typedef struct {
    uint64_t completed[4];
    uint64_t mainCount;
    uint64_t measurementStartCount;
    uint64_t slowCount;
    uint64_t *mainBins;
    uint64_t *measurementStartBins;
    uint64_t *crossWarmupBins;
    uint64_t *warmupBins;
    uint64_t *afterWindowBins;
    SlowCompletion *slowRecords;
    bool overflow;
} CompletionCounters;

bool validateWindow(const MeasurementWindow *window);
bool beginRequest(const MeasurementWindow *window, RequestState *request,
                  uint64_t attemptNs);
bool allowRequestWrite(const MeasurementWindow *window, uint64_t attemptNs);
CompletionClass classifyCompletion(const MeasurementWindow *window,
                                   uint64_t startNs, uint64_t completeNs);
bool recordCompletion(const MeasurementWindow *window, CompletionCounters *counters,
                      RequestState *request, uint64_t completeNs,
                      uint32_t status, uint64_t bodyBytes);
bool calculateCorrectionInterval(uint64_t durationNs, uint64_t completedCount,
                                 uint64_t connectionCount, uint64_t *intervalUs);
bool acceptResponseHeaders(RequestState *request, uint32_t status, uint64_t contentLength);
bool acceptResponseBody(RequestState *request, const unsigned char *bytes, uint64_t byteCount);
bool completeResponse(const MeasurementWindow *window, CompletionCounters *counters,
                      RequestState *request, uint64_t completeNs);
WindowStopAction markWindowStop(RequestState *request);
#endif
