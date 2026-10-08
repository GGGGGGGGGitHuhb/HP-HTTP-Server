#ifndef HP_S4_TAIL_WIRE_V3_H
#define HP_S4_TAIL_WIRE_V3_H
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
enum { HP_S4_INITIALIZING, HP_S4_REGISTERING, HP_S4_FROZEN, HP_S4_RUNNING,
       HP_S4_DRAINING, HP_S4_FINISHED, HP_S4_ABORTED };
enum { HP_S4_CONNECTION_LIMIT = 1, HP_S4_WORKER_MISSING, HP_S4_WORKER_RANGE,
       HP_S4_DUPLICATE_REGISTRATION, HP_S4_CLOSED_BEFORE_GO, HP_S4_REQUEST_BEFORE_GO,
       HP_S4_TUPLE_FAILURE, HP_S4_TRANSPORT_FAILURE };
typedef struct {
  uint32_t published, endpoint, code, phase;
  uint32_t expected, registered, attempts, worker;
  HpS4Connection connection;
  int32_t savedErrno;
  uint32_t reserved;
} HpS4Failure;
typedef struct {
  uint64_t timeNs;
  uint32_t phase, expected, registered, index, endpoint, ready;
} HpS4Registration;
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
  uint32_t phase, requestsPerConnection, completedConnections, registrationLock;
  uint32_t serverAttempts, clientAttempts, serverRegistered, clientRegistered;
  HpS4Failure firstFailure;
  HpS4Registration serverRegistrations[HP_S4_MAX_CONNECTIONS];
  HpS4Registration clientRegistrations[HP_S4_MAX_CONNECTIONS];

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
