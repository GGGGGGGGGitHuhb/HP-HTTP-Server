#include "HistogramExport.h"
#include "MeasurementWindow.h"
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <unistd.h>

static bool validateHistogramMetadata(const stats *histogram, bool correctionApplied) {
    if (!histogram || histogram->limit != HP_BASELINE_MAX_LATENCY_US + 1) return false;
    uint64_t total = 0;
    uint64_t minimum = UINT64_MAX;
    uint64_t maximum = 0;
    for (uint64_t bin = 0; bin < histogram->limit; bin++) {
        uint64_t count = histogram->data[bin];
        if (count > UINT64_MAX - total) return false;
        total += count;
        if (count) {
            if (minimum == UINT64_MAX) minimum = bin;
            maximum = bin;
        }
    }
    return total == histogram->count && maximum == histogram->max &&
           (correctionApplied ? minimum <= histogram->min : minimum == histogram->min);
}

bool validateHistogram(const stats *histogram) {
    return validateHistogramMetadata(histogram, false);
}

bool copyHistogram(const stats *raw, stats *copy) {
    if (!validateHistogram(raw) || !copy || copy->limit != raw->limit) return false;
    for (uint64_t bin = 0; bin < raw->limit; bin++) copy->data[bin] = raw->data[bin];
    copy->count = raw->count;
    copy->min = raw->min;
    copy->max = raw->max;
    return true;
}

bool correctHistogram(const stats *raw, stats *corrected, uint64_t intervalUs) {
    if (!intervalUs || intervalUs > INT64_MAX / 2 || !copyHistogram(raw, corrected)) return false;
    /* 扩增总量的保守上界覆盖每个桶；原算法不另加溢出行为。 */
    uint64_t maximumExpansion = raw->max / intervalUs + 1;
    if (raw->count && maximumExpansion > UINT64_MAX / raw->count) return false;
    stats_correct(corrected, (int64_t)intervalUs);
    return corrected->min == raw->min && corrected->max == raw->max &&
           validateHistogramMetadata(corrected, true);
}

static bool writeHistogramBytes(int outputFd, const unsigned char *bytes, size_t size) {
    size_t written = 0;
    while (written < size) {
        ssize_t result = write(outputFd, bytes + written, size - written);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) return false;
        written += (size_t)result;
    }
    return true;
}

static void encodeHistogramInteger(unsigned char *bytes, uint64_t value) {
    for (unsigned int offset = 0; offset < 8; offset++) bytes[offset] = (unsigned char)(value >> (offset * 8));
}

bool exportHistogram(int outputFd, const stats *histogram) {
    /* corrected 保留原 raw min，原 stats_percentile 也以此为扫描下界。 */
    if (outputFd < 0 || !validateHistogramMetadata(histogram, true)) return false;
    uint64_t nonzero = 0;
    for (uint64_t bin = 0; bin < histogram->limit; bin++) {
        if (histogram->data[bin]) nonzero++;
    }
    if (nonzero > HP_BASELINE_MAX_NONZERO_BINS) return false;
    for (uint64_t bin = 0; bin < histogram->limit; bin++) {
        if (!histogram->data[bin]) continue;
        unsigned char record[16];
        encodeHistogramInteger(record, bin);
        encodeHistogramInteger(record + 8, histogram->data[bin]);
        if (!writeHistogramBytes(outputFd, record, sizeof(record))) return false;
    }
    return true;
}
