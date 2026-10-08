#include <fcntl.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <openssl/sha.h>

static void baselineExportJsonString(FILE *output, const char *value) {
    fputc('"', output);
    for (const unsigned char *cursor = (const unsigned char *)(value ? value : ""); *cursor; cursor++) {
        if (*cursor == '"' || *cursor == '\\') {
            fputc('\\', output);
            fputc(*cursor, output);
        } else if (*cursor < 32) {
            fprintf(output, "\\u%04x", *cursor);
        } else {
            fputc(*cursor, output);
        }
    }
    fputc('"', output);
}

static void baselineExportFailure(thread *owners, unsigned int created) {
    const char *output = getenv("HP_BASELINE_OUTPUT_DIR");
    if (!output || output[0] != '/') {
        fprintf(stderr, "baseline failure evidence: output root unavailable\n");
        return;
    }
    int directoryFd = open(output, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directoryFd < 0) {
        fprintf(stderr, "baseline failure evidence: directory errno=%d\n", errno);
        return;
    }
    int resultFd = openat(directoryFd, "client-failure.json", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    FILE *result = resultFd < 0 ? NULL : fdopen(resultFd, "w");
    if (result) {
        fprintf(result, "{\"schema\":\"baseline-v1-failure\",\"status\":\"invalid\","
                        "\"first_error\":{\"reason\":\"%s\",\"errno_snapshot\":%d,\"errno_authoritative\":false},\"threads\":[",
                baselineFirstReason ? baselineFirstReason : "startup_or_export_failure", baselineFirstErrno);
        for (unsigned int index = 0; index < created; index++) {
            errors *observed = &owners[index].errors;
            fprintf(result, "%s{\"owner\":%u,\"connect\":%u,\"read\":%u,\"write\":%u,"
                            "\"status\":%u,\"timeout\":%u,\"N\":%"PRIu64",\"overflow\":%s}",
                    index ? "," : "", index, observed->connect, observed->read, observed->write,
                    observed->status, observed->timeout, owners[index].baselineCounters.mainCount,
                    owners[index].baselineCounters.overflow ? "true" : "false");
        }
        fprintf(result, "]}\n");
        bool failed = ferror(result);
        if (fclose(result)) failed = true;
        if (failed) fprintf(stderr, "baseline failure evidence: write/close errno=%d\n", errno);
    } else {
        if (resultFd >= 0) close(resultFd);
        fprintf(stderr, "baseline failure evidence: create errno=%d\n", errno);
    }
    if (close(directoryFd)) fprintf(stderr, "baseline failure evidence: directory close errno=%d\n", errno);
}

static bool baselineMergeHistogram(thread *owners, unsigned int index, stats *merged) {
    for (unsigned int owner = 0; owner < 2; owner++) {
        const stats *source = owners[owner].baselineHistograms[index];
        if (!validateHistogram(source) || source->count > UINT64_MAX - merged->count) return false;
        merged->count += source->count;
        for (uint64_t bin = 0; bin < source->limit; bin++) {
            if (source->data[bin] > UINT64_MAX - merged->data[bin]) return false;
            merged->data[bin] += source->data[bin];
        }
        if (source->min < merged->min) merged->min = source->min;
        if (source->max > merged->max) merged->max = source->max;
    }
    return validateHistogram(merged);
}

static bool baselineExportResult(thread *owners) {
    const char *output = getenv("HP_BASELINE_OUTPUT_DIR");
    if (!output || output[0] != '/') return false;
    int directoryFd = open(output, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directoryFd < 0) return false;
    stats *histograms[7] = {0};
    bool valid = true;
    for (unsigned int index = 0; index < 7; index++) histograms[index] = stats_alloc(HP_BASELINE_MAX_LATENCY_US);
    for (unsigned int index = 0; index < 5; index++) {
        if (!baselineMergeHistogram(owners, index, histograms[index])) valid = false;
    }
    uint64_t durationNs = baselineWindow.measurementEndNs - baselineWindow.measurementStartNs;
    uint64_t intervals[2] = {0};
    for (unsigned int index = 0; index < 2; index++) {
        if (!calculateCorrectionInterval(durationNs, histograms[index]->count, 128, &intervals[index]) ||
            !correctHistogram(histograms[index], histograms[index + 5], intervals[index])) valid = false;
    }
    const char *names[] = {"main-raw.bin", "measurement-start-raw.bin", "cross-warmup-raw.bin",
                           "warmup-raw.bin", "after-window-raw.bin", "main-corrected.bin",
                           "measurement-start-corrected.bin"};
    for (unsigned int index = 0; index < 7 && valid; index++) {
        int outputFd = openat(directoryFd, names[index], O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (outputFd < 0) { valid = false; break; }
        bool written = exportHistogram(outputFd, histograms[index]);
        if (close(outputFd)) written = false;
        if (!written) valid = false;
    }
    int resultFd = openat(directoryFd, "client.json", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    FILE *result = resultFd < 0 ? NULL : fdopen(resultFd, "w");
    if (!result && resultFd >= 0) close(resultFd);
    if (result) {
        unsigned char requestDigest[SHA256_DIGEST_LENGTH];
        char requestHex[SHA256_DIGEST_LENGTH * 2 + 1];
        SHA256((const unsigned char *)owners[0].cs[0].request, owners[0].cs[0].length, requestDigest);
        for (unsigned int offset = 0; offset < SHA256_DIGEST_LENGTH; offset++)
            snprintf(requestHex + offset * 2, 3, "%02x", requestDigest[offset]);
        Dl_info library = {0};
        if (!dladdr((void *)luaL_newstate, &library) || !library.dli_fname) valid = false;
        fprintf(result, "{\"pid\":%ld,\"uid\":%ld,\"gid\":%ld,\"request_bytes\":%zu,\"request_sha256\":\"%s\",\"actual_luajit_library\":",
                (long)getpid(), (long)getuid(), (long)getgid(), owners[0].cs[0].length, requestHex);
        baselineExportJsonString(result, library.dli_fname);
        fprintf(result, ",\"actual_LD_LIBRARY_PATH\":");
        baselineExportJsonString(result, getenv("LD_LIBRARY_PATH"));
        fprintf(result, ",\"actual_LUA_PATH\":");
        baselineExportJsonString(result, getenv("LUA_PATH"));
        fputc(',', result);
        fprintf(result, "\"schema\":\"baseline-v1-client\",\"status\":\"%s\",\"clock\":\"CLOCK_MONOTONIC\","
                        "\"warm_start_ns\":%"PRIu64",\"T0_ns\":%"PRIu64",\"T1_ns\":%"PRIu64","
                        "\"duration_ns\":%"PRIu64",\"N\":%"PRIu64",\"qps\":%.12Lf,\"histograms\":[",
                valid ? "valid" : "invalid", baselineWindow.warmStartNs,
                baselineWindow.measurementStartNs, baselineWindow.measurementEndNs, durationNs,
                histograms[0]->count, (long double)histograms[0]->count * 1000000000 / durationNs);
        for (unsigned int index = 0; index < 7; index++) {
            const stats *histogram = histograms[index];
            fprintf(result, "%s{\"file\":\"%s\",\"count\":%"PRIu64",\"scan_min_us\":%"PRIu64","
                            "\"max_us\":%"PRIu64",\"p50_us\":%"PRIu64",\"p99_us\":%"PRIu64"}",
                    index ? "," : "", names[index], histogram->count, histogram->min, histogram->max,
                    stats_percentile(histograms[index], 50), stats_percentile(histograms[index], 99));
        }
        fprintf(result, "],\"interval_us\":[%"PRIu64",%"PRIu64"],\"threads\":[", intervals[0], intervals[1]);
        for (unsigned int owner = 0; owner < 2; owner++) {
            CompletionCounters *counters = &owners[owner].baselineCounters;
            fprintf(result, "%s{\"owner\":%u,\"stopped_ns\":%"PRIu64",\"completed\":[%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64"],"
                            "\"pending_at_T1\":%"PRIu64",\"pending_warm\":%"PRIu64",\"censored_partial\":%"PRIu64","
                            "\"censored_unsent\":%"PRIu64",\"censored_sent_deadline_after_T1\":%"PRIu64",\"overflow\":%s,"
                            "\"errors\":{\"connect\":%u,\"read\":%u,\"write\":%u,\"status\":%u,\"timeout\":%u,"
                            "\"headers\":%"PRIu64",\"body\":%"PRIu64",\"protocol\":%"PRIu64"},\"connections\":[",
                    owner ? "," : "", owner, owners[owner].baselineStoppedNs,
                    counters->completed[0], counters->completed[1], counters->completed[2], counters->completed[3],
                    owners[owner].baselinePending, owners[owner].baselinePendingWarm,
                    owners[owner].baselineCensoredPartial, owners[owner].baselineCensoredUnsent, owners[owner].baselineCensoredSent,
                    counters->overflow ? "true" : "false", owners[owner].errors.connect, owners[owner].errors.read,
                    owners[owner].errors.write, owners[owner].errors.status, owners[owner].errors.timeout,
                    owners[owner].baselineHeaderErrors, owners[owner].baselineBodyErrors, owners[owner].baselineProtocolErrors);
            for (uint64_t connectionIndex = 0; connectionIndex < 64; connectionIndex++) {
                connection *c = &owners[owner].cs[connectionIndex];
                RequestState *request = &c->baselineRequest;
                fprintf(result, "%s{\"life\":%"PRIu64",\"sequence\":%"PRIu64",\"ready_ns\":%"PRIu64","
                                "\"pending_at_T1\":%s,\"start_ns\":%"PRIu64",\"sent_bytes\":%"PRIu64","
                                "\"full_sent\":%s,\"wait_lower_bound_ns\":%"PRIu64",\"active\":%s,"
                                "\"end_reason\":%u,\"closed\":%s}",
                        connectionIndex ? "," : "", request->lifetime, request->sequence, c->baselineReadyNs,
                        request->pendingAtWindowEnd ? "true" : "false", request->firstWriteAttemptNs, request->sentBytes,
                        request->requestFullySent ? "true" : "false",
                        request->pendingAtWindowEnd ? baselineWindow.measurementEndNs - request->firstWriteAttemptNs : 0,
                        request->requestStarted ? "true" : "false", c->baselineEndReason,
                        c->baselineClosed ? "true" : "false");
            }
            uint64_t censoredByStartPhase[2][3] = {{0}};
            for (uint64_t index = 0; index < 64; index++) {
                connection *c = &owners[owner].cs[index];
                if (c->baselineEndReason != 1 && c->baselineEndReason != 4) continue;
                unsigned int phase = c->baselineRequest.firstWriteAttemptNs < baselineWindow.measurementStartNs ? 0 : 1;
                unsigned int state = c->baselineEndReason == 4 ? 2 : (c->baselineRequest.sentBytes == 0 ? 0 : 1);
                censoredByStartPhase[phase][state]++;
            }
            fprintf(result, "],\"censored_by_start_phase\":[[%"PRIu64",%"PRIu64",%"PRIu64"],[%"PRIu64",%"PRIu64",%"PRIu64"]],"
                            "\"slow_capacity\":%u,\"slow_records\":[",
                    censoredByStartPhase[0][0], censoredByStartPhase[0][1], censoredByStartPhase[0][2],
                    censoredByStartPhase[1][0], censoredByStartPhase[1][1], censoredByStartPhase[1][2], HP_BASELINE_SLOW_CAPACITY);
            for (uint64_t index = 0; index < counters->slowCount; index++) {
                SlowCompletion *record = &counters->slowRecords[index];
                fprintf(result, "%s{\"life\":%"PRIu64",\"sequence\":%"PRIu64",\"start_ns\":%"PRIu64","
                                "\"complete_ns\":%"PRIu64",\"class\":%u,\"status\":%u,\"body_bytes\":%"PRIu64"}",
                        index ? "," : "", record->lifetime, record->sequence, record->startNs, record->completeNs,
                        record->completionClass, record->status, record->bodyBytes);
            }
            fprintf(result, "]}");
        }
        fprintf(result, "]}\n");
        if (ferror(result)) valid = false;
        if (fclose(result)) valid = false;
    } else valid = false;
    for (unsigned int index = 0; index < 7; index++) stats_free(histograms[index]);
    if (close(directoryFd)) valid = false;
    return valid;
}
