#pragma once

#include <array>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <ostream>
#include <string_view>
#include <thread>
#include <vector>

namespace hp::base {

enum class LogLevel { Info, Warn, Error };

struct LogStats {
  std::uint64_t submitted = 0;
  std::uint64_t accepted = 0;
  std::uint64_t dropped_full = 0;
  std::uint64_t rejected_stopped = 0;
  std::uint64_t truncated = 0;
  std::uint64_t written = 0;
  std::uint64_t failed = 0;
  std::uint64_t pending = 0;
};

class AsyncLogger {
 public:
  static constexpr std::size_t capacity = 1024;
  static constexpr std::size_t message_limit = 1024;

  AsyncLogger();
  ~AsyncLogger();
  AsyncLogger(const AsyncLogger&) = delete;
  AsyncLogger& operator=(const AsyncLogger&) = delete;

  void submit(LogLevel level, std::string_view message);
  void stop();
  LogStats stats() const;

 private:
  friend struct AsyncLoggerTestAccess;
  struct Record {
    LogLevel level = LogLevel::Info;
    std::size_t length = 0;
    std::array<char, message_limit> message{};
  };

  // Only test access may replace the sink or reduce capacity.
  AsyncLogger(std::size_t slots, std::ostream& sink);
  void consume();

  std::vector<Record> slots_;
  std::ostream& sink_;
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::size_t head_ = 0;
  std::size_t size_ = 0;
  bool accepting_ = true;
  LogStats stats_;

  std::mutex join_mutex_;
  std::thread consumer_;
};

class LoggerSession {
 public:
  LoggerSession();
  ~LoggerSession();
  LoggerSession(const LoggerSession&) = delete;
  LoggerSession& operator=(const LoggerSession&) = delete;

  void stop();
  LogStats stats() const;

 private:
  std::shared_ptr<AsyncLogger> logger_;
};

}  // namespace hp::base
