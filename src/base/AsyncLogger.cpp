#include "base/AsyncLogger.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace hp::base {

AsyncLogger::AsyncLogger() : AsyncLogger(kCapacity, std::clog) {}

AsyncLogger::AsyncLogger(std::size_t slots, std::ostream& sink) : slots_(slots), sink_(sink) {
  if (slots == 0 || slots > kCapacity) throw std::invalid_argument("invalid logger capacity");
  consumer_ = std::thread(&AsyncLogger::consumeRecords, this);
}

AsyncLogger::~AsyncLogger() { stopAsyncLogging(); }

void AsyncLogger::submitLogRecord(LogLevel level, std::string_view message) {
  const std::lock_guard lock(mutex_);
  ++stats_.submitted;
  if (!accepting_) {
    ++stats_.rejectedStopped;
    return;
  }
  if (size_ == slots_.size()) {
    ++stats_.droppedFull;
    return;
  }

  auto& record = slots_[(head_ + size_) % slots_.size()];
  record.level = level;
  record.length = std::min(message.size(), kMessageLimit);
  if (record.length != 0) std::memcpy(record.message.data(), message.data(), record.length);
  if (message.size() > kMessageLimit) {
    constexpr std::string_view kTruncationMarker = "...[truncated]";
    std::memcpy(record.message.data() + kMessageLimit - kTruncationMarker.size(),
                kTruncationMarker.data(),
                kTruncationMarker.size());
    ++stats_.truncated;
  }
  ++size_;
  ++stats_.accepted;
  ++stats_.pending;
  ready_.notify_one();
}

bool AsyncLogger::recordsReady() const noexcept { return size_ != 0 || !accepting_; }

void AsyncLogger::consumeRecords() {
  std::array<Record, kBatchRecords> recordBatch;
  static_assert(sizeof(recordBatch) + kBatchRecords * kOutputRecordLimit <= 140 * 1024);

  for (;;) {
    std::size_t batchCount;
    {
      std::unique_lock lock(mutex_);
      ready_.wait(lock, [this] { return recordsReady(); });
      if (size_ == 0) return;
      batchCount = std::min(size_, kBatchRecords);
      for (std::size_t index = 0; index < batchCount; ++index) {
        recordBatch[index] = slots_[head_];
        head_ = (head_ + 1) % slots_.size();
      }
      size_ -= batchCount;
    }

    const bool written = writeRecordBatch(recordBatch.data(), batchCount);
    {
      const std::lock_guard lock(mutex_);
      if (written)
        stats_.written += batchCount;
      else
        stats_.failed += batchCount;
      stats_.pending -= batchCount;
    }
  }
}

bool AsyncLogger::writeRecordBatch(const Record* records, std::size_t count) {
  std::array<char, kBatchRecords * kOutputRecordLimit> outputBatch;
  std::size_t outputLength = 0;
  for (std::size_t index = 0; index < count; ++index) {
    const auto& record = records[index];
    const std::string_view prefix = record.level == LogLevel::kInfo   ? "[INFO] "
                                    : record.level == LogLevel::kWarn ? "[WARN] "
                                                                      : "[ERROR] ";
    std::memcpy(outputBatch.data() + outputLength, prefix.data(), prefix.size());
    outputLength += prefix.size();
    std::memcpy(outputBatch.data() + outputLength, record.message.data(), record.length);
    outputLength += record.length;
    outputBatch[outputLength++] = '\n';
  }

  try {
    sink_.write(outputBatch.data(), outputLength);
    sink_.flush();
    return static_cast<bool>(sink_);
  } catch (...) {
    // 输出目标一旦失败便保持失败；不得重试或递归记录日志。
    return false;
  }
}

void AsyncLogger::stopAsyncLogging() {
  {
    const std::lock_guard lock(mutex_);
    accepting_ = false;
    ready_.notify_one();
  }
  const std::lock_guard joinLock(joinMutex_);
  if (consumer_.joinable()) consumer_.join();
}

LogStats AsyncLogger::stats() const {
  const std::lock_guard lock(mutex_);
  return stats_;
}

}  // namespace hp::base
