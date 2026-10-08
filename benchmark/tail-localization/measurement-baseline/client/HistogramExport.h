#ifndef HP_BASELINE_HISTOGRAM_EXPORT_H_
#define HP_BASELINE_HISTOGRAM_EXPORT_H_
#include <stdbool.h>
#include <stdint.h>
#include "stats.h"

#define HP_BASELINE_MAX_NONZERO_BINS 262144U

bool validateHistogram(const stats *histogram);
bool copyHistogram(const stats *raw, stats *copy);
bool correctHistogram(const stats *raw, stats *corrected, uint64_t intervalUs);
bool exportHistogram(int outputFd, const stats *histogram);
#endif
