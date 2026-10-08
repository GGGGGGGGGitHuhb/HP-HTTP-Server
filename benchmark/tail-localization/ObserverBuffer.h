#pragma once

#include <sys/mman.h>
#include <time.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace tail_localization {

enum class EventKind : std::uint16_t {
  kClientWriteBegin = 1,
  kClientWriteComplete,
  kClientFirstByte,
  kClientComplete,
  kServerCallbackBegin,
  kServerReceiveBegin,
  kServerReceiveReturn,
  kServerParseComplete,
  kServerResponsePrepared,
  kServerResponseEnqueued,
  kServerSendBegin,
  kServerSendReturn,
  kServerSendFileBegin,
  kServerSendFileReturn,
  kServerOutputDrained,
  kServerHandlerReturn,
};

struct EventRecord {
  std::uint64_t timeNs;
  std::uint64_t value;
  std::uint32_t connectionId;
  std::uint32_t requestSequence;
  EventKind kind;
  std::uint16_t flags;
  std::int32_t result;
};

static_assert(sizeof(EventRecord) == 32);

// One owner thread writes each buffer; readers are admitted after owner exit.
// Capacity allocation and page faults happen before the warmup barrier.
class ObserverBuffer {
 public:
  explicit ObserverBuffer(std::size_t capacityBytes)
      : capacityBytes_(capacityBytes), capacity_(capacityBytes / sizeof(EventRecord)) {
    if (capacityBytes == 0 || capacityBytes % sizeof(EventRecord) != 0) {
      throw std::invalid_argument("invalid observer capacity");
    }
    void* memory =
        ::mmap(nullptr, capacityBytes_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) throw std::runtime_error("observer mmap failed");
    records_ = static_cast<EventRecord*>(memory);
    auto* bytes = static_cast<volatile unsigned char*>(memory);
    for (std::size_t index = 0; index < capacityBytes_; index += 4096) bytes[index] = 0;
    bytes[capacityBytes_ - 1] = 0;
  }

  ~ObserverBuffer() {
    if (records_ != nullptr) ::munmap(records_, capacityBytes_);
  }

  ObserverBuffer(const ObserverBuffer&) = delete;
  ObserverBuffer& operator=(const ObserverBuffer&) = delete;

  void appendEvent(EventKind kind,
                   std::uint32_t connectionId,
                   std::uint32_t requestSequence,
                   std::uint64_t value,
                   std::int32_t result,
                   std::uint16_t flags) noexcept {
    if (!enabled_ || invalid_.load(std::memory_order_relaxed) ||
        overflow_.load(std::memory_order_relaxed))
      return;
    if (count_ == capacity_) {
      overflow_.store(true, std::memory_order_release);
      return;
    }
    const int savedErrno = errno;
    timespec timestamp{};
    if (::clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) {
      invalid_.store(true, std::memory_order_release);
      errno = savedErrno;
      return;
    }
    records_[count_++] = EventRecord{static_cast<std::uint64_t>(timestamp.tv_sec) * 1000000000ULL +
                                         static_cast<std::uint64_t>(timestamp.tv_nsec),
                                     value,
                                     connectionId,
                                     requestSequence,
                                     kind,
                                     flags,
                                     result};
    errno = savedErrno;
  }

  // Called by the owner before it can invoke a connection callback.
  void setEnabledBeforeCallbacks(bool enabled) noexcept { enabled_ = enabled; }

  const EventRecord* recordsAfterWriterStopped() const noexcept { return records_; }
  std::size_t countAfterWriterStopped() const noexcept { return count_; }
  bool overflowWhileWriterActive() const noexcept {
    return overflow_.load(std::memory_order_acquire);
  }
  bool invalidWhileWriterActive() const noexcept {
    return invalid_.load(std::memory_order_acquire);
  }

 private:
  EventRecord* records_{nullptr};
  std::size_t capacityBytes_;
  std::size_t capacity_;
  std::size_t count_{0};
  bool enabled_{false};
  std::atomic<bool> overflow_{false};
  std::atomic<bool> invalid_{false};
};

}  // namespace tail_localization
