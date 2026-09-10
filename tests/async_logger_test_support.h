#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <thread>

#include "base/async_logger.h"

namespace hp::base {
struct AsyncLoggerTestAccess {
  static std::size_t allocation_bytes() {
    return sizeof(AsyncLogger::Record) * AsyncLogger::capacity;
  }

  static std::unique_ptr<AsyncLogger> make(std::size_t capacity,
                                           std::ostream& sink) {
    return std::unique_ptr<AsyncLogger>(new AsyncLogger(capacity, sink));
  }
};
}  // namespace hp::base

namespace logger_test {
using namespace std::chrono_literals;

inline void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

class Gate : public std::streambuf {
 public:
  bool blocked = false;
  bool fail_write = false;
  bool fail_flush = false;
  bool throw_write = false;

  bool await_entry() {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, 3s, [this] { return entered_; });
  }

  void release() {
    const std::lock_guard lock(mutex_);
    released_ = true;
    cv_.notify_all();
  }

  std::string text() {
    const std::lock_guard lock(mutex_);
    return text_;
  }

  std::set<std::thread::id> writers() {
    const std::lock_guard lock(mutex_);
    return writers_;
  }

  std::size_t flushes() {
    const std::lock_guard lock(mutex_);
    return flushes_;
  }

 protected:
  std::streamsize xsputn(const char* data, std::streamsize size) override {
    std::unique_lock lock(mutex_);
    writers_.insert(std::this_thread::get_id());
    entered_ = true;
    cv_.notify_all();
    if (blocked) cv_.wait(lock, [this] { return released_; });
    if (throw_write) throw std::runtime_error("injected sink exception");
    if (fail_write) return 0;
    text_.append(data, size);
    return size;
  }

  int overflow(int ch) override {
    if (traits_type::eq_int_type(ch, traits_type::eof()))
      return traits_type::not_eof(ch);
    const char value = traits_type::to_char_type(ch);
    return xsputn(&value, 1) == 1 ? ch : traits_type::eof();
  }

  int sync() override {
    const std::lock_guard lock(mutex_);
    ++flushes_;
    return fail_flush ? -1 : 0;
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool entered_ = false;
  bool released_ = false;
  std::string text_;
  std::set<std::thread::id> writers_;
  std::size_t flushes_ = 0;
};

struct Fixture {
  Gate gate;
  std::ostream sink{&gate};
  std::unique_ptr<hp::base::AsyncLogger> logger;

  explicit Fixture(std::size_t capacity = 1024, bool blocked = false) {
    gate.blocked = blocked;
    logger = hp::base::AsyncLoggerTestAccess::make(capacity, sink);
  }

  ~Fixture() {
    gate.release();
    logger->stop();
  }
};

inline void balanced(const hp::base::LogStats& s) {
  require(s.submitted == s.accepted + s.dropped_full + s.rejected_stopped,
          "submitted accounting");
  require(s.accepted == s.written + s.failed + s.pending,
          "accepted accounting");
}
}  // namespace logger_test
