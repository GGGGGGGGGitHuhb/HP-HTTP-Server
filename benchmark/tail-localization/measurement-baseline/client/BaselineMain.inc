int main(int argc, char **argv) {
    char *url = NULL;
    char **headers = zcalloc((size_t)argc * sizeof(char *));
    struct http_parser_url parts = {0};
    if (parse_args(&cfg, &url, &parts, headers, argc, argv)) return 2;
    char *schema = copy_url_part(url, &parts, UF_SCHEMA);
    char *host = copy_url_part(url, &parts, UF_HOST);
    char *port = copy_url_part(url, &parts, UF_PORT);
    if (!schema || strcmp(schema, "http") || !host || strcmp(host, "127.0.0.1") || !port ||
        cfg.connections != 128 || cfg.threads != 2 || cfg.timeout != 2000 || cfg.script ||
        (cfg.duration != 20 && cfg.duration != 3) || headers[0] ||
        !(parts.field_set & (1 << UF_PATH)) ||
        strcmp(url + parts.field_data[UF_PATH].off, "/payload-1024.bin")) return 2;
    const char *warm = getenv("HP_BASELINE_WARMUP_SECONDS");
    if (!warm || ((cfg.duration == 20 && strcmp(warm, "5")) ||
                  (cfg.duration == 3 && strcmp(warm, "1")))) return 2;
    struct addrinfo hints = {0}, *address = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    if (getaddrinfo(host, port, &hints, &address) || !address || address->ai_next) return 2;
    cfg.host = host;
    signal(SIGPIPE, SIG_IGN);
    parser_settings.on_headers_complete = baselineHeadersComplete;
    parser_settings.on_body = response_body;
    thread *owners = zcalloc(2 * sizeof(thread));
    unsigned int created = 0;
    for (unsigned int index = 0; index < 2; index++) {
        owners[index].addr = address;
        owners[index].connections = 64;
        owners[index].loop = aeCreateEventLoop(394);
        if (!owners[index].loop || pthread_create(&owners[index].thread, NULL, thread_main, &owners[index])) {
            __atomic_store_n(&baselineFailed, 1, __ATOMIC_RELEASE);
            break;
        }
        created++;
    }
    uint64_t readyDeadline = baselineClockNs() + 5000000000;
    while (!__atomic_load_n(&baselineFailed, __ATOMIC_ACQUIRE) &&
           __atomic_load_n(&baselineReadyCount, __ATOMIC_ACQUIRE) != 128) {
        if (baselineClockNs() >= readyDeadline) {
            __atomic_store_n(&baselineFailed, 1, __ATOMIC_RELEASE);
            fprintf(stderr, "baseline first failure: connections not ready\n");
            break;
        }
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    if (!__atomic_load_n(&baselineFailed, __ATOMIC_ACQUIRE)) {
        baselineWindow.warmStartNs = baselineClockNs() + 20000000;
        baselineWindow.measurementStartNs = baselineWindow.warmStartNs +
                                           (cfg.duration == 20 ? 5000000000 : 1000000000);
        baselineWindow.measurementEndNs = baselineWindow.measurementStartNs + cfg.duration * 1000000000;
        __atomic_store_n(&baselineGatePublished, 1, __ATOMIC_RELEASE);
    }
    for (unsigned int index = 0; index < created; index++) {
        if (pthread_join(owners[index].thread, NULL)) __atomic_store_n(&baselineFailed, 1, __ATOMIC_RELEASE);
    }
    bool valid = created == 2 && !__atomic_load_n(&baselineFailed, __ATOMIC_ACQUIRE);
    if (valid) valid = baselineExportResult(owners);
    if (!valid) baselineExportFailure(owners, created);
    for (unsigned int index = 0; index < created; index++) {
        for (unsigned int histogram = 0; histogram < 5; histogram++) stats_free(owners[index].baselineHistograms[histogram]);
        zfree(owners[index].baselineCounters.slowRecords);
        zfree(owners[index].cs);
    }
    freeaddrinfo(address);
    zfree(owners);
    zfree(headers);
    zfree(schema);
    zfree(host);
    zfree(port);
    return valid ? 0 : 1;
}
