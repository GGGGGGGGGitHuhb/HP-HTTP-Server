#ifndef HP_S4_CLOCK_EVIDENCE_H
#define HP_S4_CLOCK_EVIDENCE_H
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t evidenceMonotonicNs(void) {
  struct timespec value;
  if (clock_gettime(CLOCK_MONOTONIC, &value)) return 0;
  return (uint64_t)value.tv_sec * 1000000000ULL + value.tv_nsec;
}

/* 仅启动阶段读取；请求热路径不调用。 */
static int saveClockEvidence(const char *directory, const char *endpoint) {
  char boot[80] = {0}, namespaceName[128] = {0}, path[4096];
  uint64_t bootBegin = evidenceMonotonicNs();
  int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  ssize_t size = read(fd, boot, sizeof(boot) - 1);
  close(fd);
  uint64_t bootEnd = evidenceMonotonicNs();
  if (size <= 0) return -1;
  boot[strcspn(boot, "\r\n")] = 0;
  uint64_t namespaceBegin = evidenceMonotonicNs();
  size = readlink("/proc/self/ns/time", namespaceName, sizeof(namespaceName) - 1);
  uint64_t namespaceEnd = evidenceMonotonicNs();
  if (size <= 0) return -1;
  struct timespec resolution;
  uint64_t resolutionBegin = evidenceMonotonicNs();
  if (clock_getres(CLOCK_MONOTONIC, &resolution)) return -1;
  uint64_t resolutionEnd = evidenceMonotonicNs();
  uint64_t minimum = UINT64_MAX, maximum = 0;
  for (unsigned int index = 0; index < 32; ++index) {
    uint64_t before = evidenceMonotonicNs(), after = evidenceMonotonicNs();
    if (!before || after < before) return -1;
    uint64_t delta = after - before;
    if (delta < minimum) minimum = delta;
    if (delta > maximum) maximum = delta;
  }
  if (snprintf(path, sizeof(path), "%s/%s-clock.json", directory, endpoint) >= (int)sizeof(path)) return -1;
  fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) return -1;
  int result = dprintf(fd,
      "{\"clock\":\"CLOCK_MONOTONIC\",\"unit\":\"ns\",\"pid\":%ld,"
      "\"boot_id\":\"%s\",\"time_namespace\":\"%s\","
      "\"boot_read_ns\":[%llu,%llu],\"namespace_read_ns\":[%llu,%llu],"
      "\"resolution_read_ns\":[%llu,%llu],\"resolution_ns\":%llu,"
      "\"clock_pair_count\":32,\"clock_pair_min_ns\":%llu,\"clock_pair_max_ns\":%llu}\n",
      (long)getpid(), boot, namespaceName,
      (unsigned long long)bootBegin, (unsigned long long)bootEnd,
      (unsigned long long)namespaceBegin, (unsigned long long)namespaceEnd,
      (unsigned long long)resolutionBegin, (unsigned long long)resolutionEnd,
      (unsigned long long)((uint64_t)resolution.tv_sec * 1000000000ULL + resolution.tv_nsec),
      (unsigned long long)minimum, (unsigned long long)maximum);
  int closed = close(fd);
  return result > 0 && closed == 0 ? 0 : -1;
}
#endif
