#ifndef HP_S4_STARTUP_EVIDENCE_H
#define HP_S4_STARTUP_EVIDENCE_H
#include "TailWire.h"
/* 仅失败路径争用首因槽；published=2 表示字段已全部发布。 */
static inline void publishStartupFailure(HpS4Control *control, uint32_t endpoint,
                                        uint32_t code, const HpS4Connection *connection,
                                        int32_t savedErrno, uint32_t triggerPhase) {
  uint32_t empty = 0;
  if (__atomic_compare_exchange_n(&control->firstFailure.published, &empty, 1U, 0,
                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    HpS4Failure *failure = &control->firstFailure;
    failure->endpoint = endpoint;
    failure->code = code;
    failure->phase = triggerPhase;
    failure->expected = control->connections;
    failure->registered = __atomic_load_n(endpoint ? &control->clientRegistered : &control->serverRegistered, __ATOMIC_ACQUIRE);
    failure->attempts = __atomic_load_n(endpoint ? &control->clientAttempts : &control->serverAttempts, __ATOMIC_ACQUIRE);
    failure->worker = connection->worker;
    failure->connection = *connection;
    failure->savedErrno = savedErrno;
    __atomic_store_n(&failure->published, 2U, __ATOMIC_RELEASE);
  }
  __atomic_store_n(&control->phase, HP_S4_ABORTED, __ATOMIC_RELEASE);
  __atomic_store_n(&control->abortRun, 1U, __ATOMIC_RELEASE);
}
#endif
