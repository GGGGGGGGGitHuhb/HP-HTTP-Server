#define _GNU_SOURCE
#include "ClientObserver.h"
#include "ClockEvidence.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

typedef struct {
  HpS4Event *records;
  uint64_t count;
  uint32_t overflow;
} ClientBuffer;
static HpS4Control *control;
static ClientBuffer buffers[HP_S4_CLIENT_THREADS];
static const char *outputDirectory;

uint64_t clientMonotonicNs(void) {
  struct timespec value;
  if (clock_gettime(CLOCK_MONOTONIC, &value)) abort();
  return (uint64_t)value.tv_sec * 1000000000ULL + value.tv_nsec;
}

static uint64_t threadStarttime(uint32_t tid) {
  char path[80], line[4096];
  snprintf(path, sizeof(path), "/proc/self/task/%u/stat", tid);
  FILE *input = fopen(path, "r");
  if (!input || !fgets(line, sizeof(line), input)) abort();
  fclose(input);
  char *cursor = strrchr(line, ')');
  if (!cursor) abort();
  cursor += 2;
  char *saved = NULL, *field = strtok_r(cursor, " ", &saved);
  for (int index = 3; index < 22 && field; ++index) field = strtok_r(NULL, " ", &saved);
  if (!field) abort();
  return strtoull(field, NULL, 10);
}

static void registerIdentity(HpS4Thread *identity, uint32_t worker, const char *role) {
  identity->pid = getpid();
  identity->tid = syscall(SYS_gettid);
  identity->worker = worker;
  identity->starttime = threadStarttime(identity->tid);
  const char *marker = getenv("HP_S4_MARKER_FD");
  if (marker) {
    char message[192];
    int count = snprintf(message, sizeof(message), "S4MARK client %s %u %u %u %llu\n", role, worker,
                         identity->pid, identity->tid, (unsigned long long)identity->starttime);
    if (count <= 0 || write(atoi(marker), message, count) != count) abort();
  }
  __atomic_store_n(&identity->ready, 1U, __ATOMIC_RELEASE);
}

void initializeClientObserver(uint32_t connections, uint32_t threads) {
  const char *path = getenv("HP_S4_CONTROL");
  outputDirectory = getenv("HP_S4_OUTPUT");
  if (!path || !outputDirectory || threads != HP_S4_CLIENT_THREADS || connections > HP_S4_MAX_CONNECTIONS ||
      connections == 0 || connections % threads) abort();
  if (saveClockEvidence(outputDirectory,"client")) abort();
  int fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
  struct stat status;
  if (fd < 0 || fstat(fd, &status) || status.st_size != sizeof(HpS4Control)) abort();
  control = mmap(NULL, sizeof(HpS4Control), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (control == MAP_FAILED || memcmp(control->magic, "S4CTRL02", 8) || control->version != 2 ||
      control->bytes != sizeof(HpS4Control) || control->connections != connections) abort();
  for (uint32_t worker = 0; worker < threads; ++worker) {
    buffers[worker].records = mmap(NULL, HP_S4_BUFFER_BYTES, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buffers[worker].records == MAP_FAILED) abort();
    volatile unsigned char *bytes = (void *)buffers[worker].records;
    for (uint32_t page = 0; page < HP_S4_BUFFER_BYTES; page += 4096) bytes[page] = 0;
    bytes[HP_S4_BUFFER_BYTES - 1] = 0;
  }
  registerIdentity(&control->clientThreads[2], 2, "main");
}

HpS4Control *clientControl(void) { return control; }

void registerClientThread(uint32_t worker) {
  if (worker >= HP_S4_CLIENT_THREADS) abort();
  registerIdentity(&control->clientThreads[worker], worker, "worker");
}

void registerClientConnection(uint32_t index, uint32_t worker, int fd) {
  if (index >= control->connections || worker >= HP_S4_CLIENT_THREADS) abort();
  struct sockaddr_in local, remote;
  socklen_t localBytes = sizeof(local), remoteBytes = sizeof(remote);
  if (getsockname(fd, (void *)&local, &localBytes) || getpeername(fd, (void *)&remote, &remoteBytes) ||
      local.sin_family != AF_INET || remote.sin_family != AF_INET) abort();
  HpS4Connection *record = &control->clientConnections[index];
  record->worker = worker;
  record->pid = getpid();
  record->tid = syscall(SYS_gettid);
  record->starttime = control->clientThreads[worker].starttime;
  record->lifetime = index + 1;
  record->localAddress = local.sin_addr.s_addr;
  record->remoteAddress = remote.sin_addr.s_addr;
  record->localPort = ntohs(local.sin_port);
  record->remotePort = ntohs(remote.sin_port);
  record->fd = fd;
  record->index = index;
  __atomic_store_n(&record->ready, 1U, __ATOMIC_RELEASE);
}

void abortClientObserver(void) { __atomic_store_n(&control->abortRun, 1U, __ATOMIC_RELEASE); }

void appendClientEvent(uint32_t worker, uint32_t index, uint32_t sequence,
                       uint16_t kind, uint64_t value, int32_t result, uint16_t flags) {
  int savedErrno = errno;
  if (!control->detailed || !__atomic_load_n(&control->go, __ATOMIC_ACQUIRE)) return;
  int selected = 0;
  for (uint32_t item = 0; item < control->selectedCount; ++item)
    if (control->selectedClient[item] == index) selected = 1;
  if (!selected) return;
  ClientBuffer *buffer = &buffers[worker];
  if (buffer->count >= HP_S4_BUFFER_BYTES / sizeof(HpS4Event)) {
    buffer->overflow = 1;
    abortClientObserver();
    return;
  }
  buffer->records[buffer->count++] = (HpS4Event){clientMonotonicNs(),value,index,sequence,kind,flags,result};
  errno = savedErrno;
}

void stopClientThread(uint32_t worker) {
  control->clientThreads[worker].stoppedNs = clientMonotonicNs();
}

void exportClientObserver(void) {
  for (uint32_t worker = 0; worker < HP_S4_CLIENT_THREADS; ++worker) {
    ClientBuffer *buffer = &buffers[worker];
    HpS4Thread *identity = &control->clientThreads[worker];
    if (!identity->ready || !identity->stoppedNs) abort();
    HpS4Header header = {0};
    memcpy(header.magic, "S4TAIL02", 8);
    header.headerBytes = sizeof(header);
    header.eventBytes = sizeof(HpS4Event);
    header.flags = 1 | (buffer->overflow ? 2 : 0);
    header.count = buffer->count;
    header.capacity = HP_S4_BUFFER_BYTES / sizeof(HpS4Event);
    header.pid = identity->pid;
    header.tid = identity->tid;
    header.starttime = identity->starttime;
    if (buffer->count) {
      header.firstNs = buffer->records[0].timeNs;
      header.lastNs = buffer->records[buffer->count - 1].timeNs;
    }
    char path[4096];
    if (snprintf(path, sizeof(path), "%s/client-worker-%u.events.bin", outputDirectory, worker) >= sizeof(path)) abort();
    FILE *output = fopen(path, "wb");
    if (!output || fwrite(&header, sizeof(header), 1, output) != 1 ||
        fwrite(buffer->records, sizeof(HpS4Event), buffer->count, output) != buffer->count || fclose(output)) abort();
    munmap(buffer->records, HP_S4_BUFFER_BYTES);
  }
  munmap(control, sizeof(HpS4Control));
  control = NULL;
}
