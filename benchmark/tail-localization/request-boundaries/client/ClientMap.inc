#include <arpa/inet.h>

static bool boundaryCaptureConnection(connection *c) {
    struct sockaddr_in local = {0}, remote = {0};
    socklen_t localSize = sizeof(local), remoteSize = sizeof(remote);
    if (getsockname(c->fd, (struct sockaddr *)&local, &localSize) ||
        getpeername(c->fd, (struct sockaddr *)&remote, &remoteSize) ||
        localSize != sizeof(local) || remoteSize != sizeof(remote) ||
        local.sin_family != AF_INET || remote.sin_family != AF_INET) return false;
    c->boundaryClientAddress = ntohl(local.sin_addr.s_addr);
    c->boundaryServerAddress = ntohl(remote.sin_addr.s_addr);
    c->boundaryClientPort = ntohs(local.sin_port);
    c->boundaryServerPort = ntohs(remote.sin_port);
    c->boundaryConnectedFd = c->fd;
    return c->boundaryClientPort && c->boundaryServerPort;
}

static uint64_t boundaryProcessStarttime(void) {
    FILE *source = fopen("/proc/self/stat", "r");
    if (!source) return 0;
    char line[4096];
    bool valid = fgets(line, sizeof(line), source) != NULL;
    if (fclose(source)) valid = false;
    if (!valid) return 0;
    char *ending = strrchr(line, ')');
    if (!ending) return 0;
    char *save = NULL;
    char *token = strtok_r(ending + 2, " ", &save);
    for (unsigned field = 3; field < 22 && token; field++) token = strtok_r(NULL, " ", &save);
    if (!token) return 0;
    char *end = NULL;
    uint64_t value = strtoull(token, &end, 10);
    return end && *end == 0 ? value : 0;
}

typedef struct {
    char bootId[64];
    char timeNamespace[128];
    uint64_t resolutionNs;
    uint64_t observedNs;
    uint64_t processStarttime;
} BoundaryClockIdentity;
static BoundaryClockIdentity boundaryClockStart;

static bool boundaryReadClockIdentity(BoundaryClockIdentity *value) {
    FILE *boot = fopen("/proc/sys/kernel/random/boot_id", "r");
    if (!boot) return false;
    bool valid = fgets(value->bootId, sizeof(value->bootId), boot) != NULL;
    if (fclose(boot)) valid = false;
    value->bootId[strcspn(value->bootId, "\n")] = 0;
    ssize_t size = readlink("/proc/self/ns/time", value->timeNamespace, sizeof(value->timeNamespace) - 1);
    if (size <= 0 || size >= (ssize_t)sizeof(value->timeNamespace) - 1) return false;
    value->timeNamespace[size] = 0;
    struct timespec resolution = {0};
    if (clock_getres(CLOCK_MONOTONIC, &resolution)) return false;
    value->resolutionNs = (uint64_t)resolution.tv_sec * 1000000000 + resolution.tv_nsec;
    value->observedNs = baselineClockNs();
    value->processStarttime = boundaryProcessStarttime();
    return valid && strlen(value->bootId) == 36 && value->resolutionNs && value->observedNs && value->processStarttime;
}

static bool boundaryInitializeIdentity(void) {
    return boundaryReadClockIdentity(&boundaryClockStart);
}

static void boundaryExportClock(FILE *result, const char *name, const BoundaryClockIdentity *value) {
    fprintf(result, "\"clock_%s\":{\"clock\":\"CLOCK_MONOTONIC\",\"boot_id\":", name);
    baselineExportJsonString(result, value->bootId);
    fprintf(result, ",\"time_namespace\":");
    baselineExportJsonString(result, value->timeNamespace);
    fprintf(result, ",\"resolution_ns\":%"PRIu64",\"observed_ns\":%"PRIu64",\"process_starttime\":%"PRIu64"},",
            value->resolutionNs, value->observedNs, value->processStarttime);
}

static bool boundaryExportMap(thread *owners) {
    const char *output = getenv("HP_BASELINE_OUTPUT_DIR");
    int directory = output ? open(output, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
    if (directory < 0) return false;
    int descriptor = openat(directory, "client-map.json", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    FILE *result = descriptor < 0 ? NULL : fdopen(descriptor, "w");
    if (!result) {
        if (descriptor >= 0) close(descriptor);
        close(directory);
        return false;
    }
    uint64_t starttime = boundaryProcessStarttime();
    if (!starttime) { fclose(result); close(directory); return false; }
    fprintf(result, "{\"schema\":\"baseline-v1-map\",\"pid\":%ld,\"process_starttime\":%"PRIu64",\"run_id\":", (long)getpid(), starttime);
    baselineExportJsonString(result, getenv("HP_BASELINE_RUN"));
    BoundaryClockIdentity clockStop = {0};
    if (!boundaryReadClockIdentity(&clockStop) || clockStop.processStarttime != boundaryClockStart.processStarttime ||
        strcmp(clockStop.bootId, boundaryClockStart.bootId) || strcmp(clockStop.timeNamespace, boundaryClockStart.timeNamespace)) {
        fclose(result); close(directory); return false;
    }
    fputc(',', result);
    boundaryExportClock(result, "start", &boundaryClockStart);
    boundaryExportClock(result, "stop", &clockStop);
    fprintf(result, "\"connections\":[");
    for (unsigned int owner = 0; owner < 2; owner++) {
        for (unsigned int index = 0; index < 64; index++) {
            connection *c = &owners[owner].cs[index];
            fprintf(result, "%s{\"owner\":%u,\"life\":%"PRIu64",\"fd\":%d,"
                            "\"client_address_u32\":%u,\"client_port\":%u,\"server_address_u32\":%u,\"server_port\":%u,"
                            "\"connected_ns\":%"PRIu64",\"final_sequence\":%"PRIu64"}",
                    owner || index ? "," : "", owner, c->baselineRequest.lifetime,
                    c->boundaryConnectedFd, c->boundaryClientAddress, c->boundaryClientPort,
                    c->boundaryServerAddress, c->boundaryServerPort, c->baselineReadyNs, c->baselineRequest.sequence);
        }
    }
    fputs("]}\n", result);
    bool valid = !ferror(result);
    if (fclose(result)) valid = false;
    if (close(directory)) valid = false;
    return valid;
}
