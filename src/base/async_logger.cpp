#include "base/async_logger.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace hp::base {

AsyncLogger::AsyncLogger() : AsyncLogger(kCapacity, std::clog) {}

AsyncLogger::AsyncLogger(std::size_t slots, std::ostream& sink)
    : slots_(slots), sink_(sink) {
  if (slots == 0 || slots > kCapacity)
    throw std::invalid_argument("invalid logger capacity");
  consumer_ = std::thread(&AsyncLogger::ConsumeRecords, this);
}

AsyncLogger::~AsyncLogger() { Stop(); }

void AsyncLogger::Submit(LogLevel level, std::string_view message) {
  const std::lock_guard lock(mutex_);
  ++stats_.submitted;
  if (!accepting_) {
    ++stats_.rejected_stopped;
    return;
  }
  if (size_ == slots_.size()) {
    ++stats_.dropped_full;
    return;
  }

  auto& record = slots_[(head_ + size_) % slots_.size()];
  record.level = level;
  record.length = std::min(message.size(), kMessageLimit);
  if (record.length != 0)
    std::memcpy(record.message.data(), message.data(), record.length);
  if (message.size() > kMessageLimit) {
    constexpr std::string_view kTruncationMarker = "...[truncated]";
    std::memcpy(
        record.message.data() + kMessageLimit - kTruncationMarker.size(),
        kTruncationMarker.data(),
        kTruncationMarker.size());
    ++stats_.truncated;
  }
  ++size_;
  ++stats_.accepted;
  ++stats_.pending;
  ready_.notify_one();
}

void AsyncLogger::ConsumeRecords() {
  for (;;) {
    Record record;
    {
      std::unique_lock lock(mutex_);
      ready_.wait(lock, [this] { return size_ != 0 || !accepting_; });
      if (size_ == 0) return;
      record = slots_[head_];
      head_ = (head_ + 1) % slots_.size();
      --size_;
    }

    bool written = false;
    try {
      const auto level = record.level == LogLevel::kInfo   ? "INFO"
                         : record.level == LogLevel::kWarn ? "WARN"
                                                           : "ERROR";
      sink_ << '[' << level << "] ";
      sink_.write(record.message.data(), record.length);
      sink_.put('\n');
      sink_.flush();
      written = static_cast<bool>(sink_);
    } catch (...) {
      // A failed sink remains failed; never retry or recursively log.
    }
    {
      const std::lock_guard lock(mutex_);
      if (written)
        ++stats_.written;
      else
        ++stats_.failed;
      --stats_.pending;
    }
  }
}

void AsyncLogger::Stop() {
  {
    const std::lock_guard lock(mutex_);
    accepting_ = false;
    ready_.notify_one();
  }
  const std::lock_guard join_lock(join_mutex_);
  if (consumer_.joinable()) consumer_.join();
}

LogStats AsyncLogger::stats() const {
  const std::lock_guard lock(mutex_);
  return stats_;
}

}  // namespace hp::base
