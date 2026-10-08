#pragma once
#include <cstddef>
#include <cstdint>
#include "TailWire.h"

namespace tail_localization {
struct ConnectionContext {
  std::uint32_t index{UINT32_MAX};
  std::uint32_t sequence{1};
  bool drained{false};
};

void initializeObserver();
void exportObserverAfterWorkersJoined();
void closeMarkerAfterStartup();
void registerWorker(std::size_t workerIndex);
void registerLoggerThread();
void stopWorker(std::size_t workerIndex) noexcept;
ConnectionContext registerConnection(int fd, std::uint64_t lifetime);
void closeConnection(ConnectionContext& context) noexcept;
ConnectionContext* exchangeCurrentConnection(ConnectionContext* context) noexcept;
void appendConnectionEvent(ConnectionContext& context, std::uint16_t kind,
                           std::uint64_t value = 0, std::int32_t result = 0,
                           std::uint16_t flags = 0) noexcept;
void appendIoEvent(std::uint16_t kind, std::uint64_t value,
                   std::int32_t result = 0, std::uint16_t flags = 0) noexcept;
void receiveSucceeded() noexcept;
void markOutputDrained(ConnectionContext& context) noexcept;
std::uint32_t prospectiveSequence(const ConnectionContext& context) noexcept;
}  // namespace tail_localization
