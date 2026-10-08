#ifndef HP_S4_CLIENT_OBSERVER_H
#define HP_S4_CLIENT_OBSERVER_H
#include "TailWire.h"
#include <stddef.h>
#include <stdint.h>
void initializeClientObserver(uint32_t connections, uint32_t threads);
void registerClientThread(uint32_t worker);
void registerClientConnection(uint32_t index, uint32_t worker, int fd);
uint64_t clientMonotonicNs(void);
HpS4Control *clientControl(void);
void appendClientEvent(uint32_t worker, uint32_t index, uint32_t sequence,
                       uint16_t kind, uint64_t value, int32_t result, uint16_t flags);
void rejectClientRequestBeforeGo(uint32_t index);
void abortClientObserver(void);
void stopClientThread(uint32_t worker);
void exportClientObserver(void);
#endif
