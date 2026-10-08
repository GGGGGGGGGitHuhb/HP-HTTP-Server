#ifndef HP_S4_TAIL_WIRE_H
#define HP_S4_TAIL_WIRE_H
#include <stdint.h>
#define HP_S4_MAX_CONNECTIONS 128
#define HP_S4_MAX_SELECTED 16
#define HP_S4_WORKERS 4
#define HP_S4_CLIENT_THREADS 2
#define HP_S4_BUFFER_BYTES (16U * 1024U * 1024U)
/* C ABI shared by the independent client, server and startup controller. */
typedef struct {
  uint32_t ready, worker, pid, tid;
  uint64_t starttime, lifetime;
  uint32_t localAddress, remoteAddress;
  uint16_t localPort, remotePort;
  int32_t fd;
  uint32_t index, closed;
  uint64_t reserved;
} HpS4Connection;
typedef struct {
  uint32_t ready, pid, tid, worker;
  uint64_t starttime;
  uint64_t stoppedNs;
} HpS4Thread;
typedef struct {
  char magic[8];
  uint32_t version, bytes, go, abortRun;
  uint32_t connections, detailed;
  uint64_t warmupNs, measurementNs, startNs;
  uint32_t selectedCount, reserved;
  uint32_t selectedServer[HP_S4_MAX_SELECTED];
  uint32_t selectedClient[HP_S4_MAX_SELECTED];
  HpS4Thread serverThreads[6];
  HpS4Thread clientThreads[3];
  HpS4Connection serverConnections[HP_S4_MAX_CONNECTIONS];
  HpS4Connection clientConnections[HP_S4_MAX_CONNECTIONS];
} HpS4Control;
typedef struct {
  uint64_t timeNs, value;
  uint32_t connectionId, requestSequence;
  uint16_t kind, flags;
  int32_t result;
} HpS4Event;
typedef struct {
  char magic[8];
  uint16_t headerBytes, eventBytes;
  uint32_t flags;
  uint64_t count, capacity;
  uint32_t pid, tid;
  uint64_t starttime, firstNs, lastNs;
} HpS4Header;
#endif
