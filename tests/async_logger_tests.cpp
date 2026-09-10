#include <atomic>
#include <barrier>
#include <future>
#include <iostream>
#include <vector>

#include "async_logger_faults.h"
#include "async_logger_test_support.h"
#include "base/logger.h"

namespace {
using namespace logger_test;
using hp::base::LogLevel;

void capacity_and_ownership() {
  Gate gate;
  std::ostream sink(&gate);
  bool invalid = false;
  try {
    auto logger = hp::base::AsyncLoggerTestAccess::make(0, sink);
  } catch (const std::invalid_argument&) {
    invalid = true;
  }
  require(invalid, "capacity zero rejected");

  Fixture f(1, true);
  f.logger->submit(LogLevel::Info, "inflight");
  require(f.gate.await_entry(), "sink entry handshake");
  std::string temporary = "owned temporary";
  auto producer = std::async(std::launch::async, [&] {
    f.logger->submit(LogLevel::Warn, temporary);
    f.logger->submit(LogLevel::Error, "drop error");
    f.logger->submit(LogLevel::Info, "drop info");
    f.logger->submit(LogLevel::Warn, "drop warn");
  });
  const bool completed = producer.wait_for(200ms) == std::future_status::ready;
  f.gate.release();
  producer.get();
  require(completed, "producer must complete before sink release");
  // A separate blocked first record makes the borrowed-message mutation exact.
  f.logger->stop();
  auto s = f.logger->stats();
  balanced(s);
  require(s.accepted == 2 && s.dropped_full == 3 && s.written == 2 &&
              s.pending == 0,
          "capacity one and all-level drop accounting");
  require(f.gate.text() == "[INFO] inflight\n[WARN] owned temporary\n",
          "FIFO and compatible text");
  require(f.gate.writers().size() == 1 &&
              !f.gate.writers().contains(std::this_thread::get_id()),
          "single consumer identity");

  Fixture owned(4, true);
  owned.logger->submit(LogLevel::Info, "gate");
  require(owned.gate.await_entry(), "ownership gate entry");
  {
    std::string message = "original message";
    owned.logger->submit(LogLevel::Info, message);
    message.assign(message.size(), 'X');
    // Keep the overwritten source alive until consumption, avoiding UB in the
    // narrow borrowed-message negative control.
    owned.gate.release();
    owned.logger->stop();
  }
  require(owned.gate.text() == "[INFO] gate\n[INFO] original message\n",
          "owned message survives source overwrite");

  Fixture full_default(1024, true);
  full_default.logger->submit(LogLevel::Info, "inflight");
  require(full_default.gate.await_entry(), "default capacity sink entry");
  for (int i = 0; i < 1024; ++i)
    full_default.logger->submit(LogLevel::Info, "slot");
  full_default.logger->submit(LogLevel::Error, "drop");
  const auto occupied = full_default.logger->stats();
  full_default.gate.release();
  full_default.logger->stop();
  require(occupied.accepted == 1025 && occupied.pending == 1025 &&
              occupied.dropped_full == 1,
          "default capacity 1024 plus one inflight");
  balanced(full_default.logger->stats());

  Fixture limits;
  limits.logger->submit(LogLevel::Info, "");
  limits.logger->submit(LogLevel::Info, std::string(1024, 'a'));
  limits.logger->submit(LogLevel::Info, std::string(1025, 'b'));
  limits.logger->stop();
  s = limits.logger->stats();
  balanced(s);
  require(s.accepted == 3 && s.truncated == 1 && s.written == 3,
          "message boundary accounting");
  require(limits.gate.text() == "[INFO] \n[INFO] " + std::string(1024, 'a') +
                                    "\n[INFO] " + std::string(1010, 'b') +
                                    "...[truncated]\n",
          "message 0 1024 1025 exact truncation");
  std::cout << "capacity ownership boundaries PASS\n";
}

void concurrent_and_failure() {
  Fixture f;
  std::vector<std::thread> producers;
  for (int p = 0; p < 4; ++p) {
    producers.emplace_back([&, p] {
      for (int i = 0; i < 1000; ++i) {
        while (f.logger->stats().pending >= 128) std::this_thread::yield();
        f.logger->submit(LogLevel::Info,
                         std::to_string(p) + ":" + std::to_string(i));
      }
    });
  }
  for (auto& producer : producers) producer.join();
  f.logger->stop();
  auto s = f.logger->stats();
  balanced(s);
  require(s.accepted == 4000 && s.written == 4000 && s.pending == 0 &&
              s.dropped_full == 0,
          "four producers 4000 no drop");
  int next[4]{};
  std::istringstream lines(f.gate.text());
  std::string line;
  while (std::getline(lines, line)) {
    int p = -1, i = -1;
    require(std::sscanf(line.c_str(), "[INFO] %d:%d", &p, &i) == 2 && p >= 0 &&
                p < 4 && i == next[p]++,
            "per producer FIFO no duplicates");
  }
  for (auto value : next) require(value == 1000, "no missing messages");
  require(f.gate.flushes() == 4000, "each message flushed");

  Fixture overload(16, true);
  overload.logger->submit(LogLevel::Info, "gate");
  require(overload.gate.await_entry(), "overload entered sink");
  producers.clear();
  for (int p = 0; p < 4; ++p) {
    producers.emplace_back([&, p] {
      for (int i = 0; i < 1000; ++i)
        overload.logger->submit(LogLevel::Error,
                                std::to_string(p) + ":" + std::to_string(i));
    });
  }
  for (auto& producer : producers) producer.join();
  const auto saturated = overload.logger->stats();
  overload.gate.release();
  overload.logger->stop();
  require(saturated.submitted == 4001 && saturated.accepted == 17 &&
              saturated.dropped_full == 3984 && saturated.pending == 17 &&
              overload.logger->stats().written == 17,
          "four producer overload exact accepted dropped pending");
  balanced(overload.logger->stats());
  std::istringstream emitted(overload.gate.text());
  std::set<std::string> unique;
  while (std::getline(emitted, line))
    require(unique.insert(line).second, "overload accepted messages unique");
  require(unique.size() == 17, "overload accepted set matches count");

  for (int mode = 0; mode < 3; ++mode) {
    Fixture failure;
    failure.gate.fail_write = mode == 0;
    failure.gate.fail_flush = mode == 1;
    failure.gate.throw_write = mode == 2;
    if (mode == 2) failure.sink.exceptions(std::ios::badbit);
    for (int i = 0; i < 20; ++i)
      failure.logger->submit(LogLevel::Error, "failure");
    failure.logger->stop();
    s = failure.logger->stats();
    balanced(s);
    require(s.failed == 20 && s.written == 0 && s.pending == 0,
            "permanent write flush exception failure drained");
  }
  std::cout
      << "concurrency 4000 FIFO and write/flush/exception failures PASS\n";
}

void lifecycle() {
  for (int i = 0; i < 100; ++i) {
    Fixture f;
    f.logger->submit(LogLevel::Info, "cycle");
    f.logger->stop();
    f.logger->stop();
    f.logger->submit(LogLevel::Error, "late");
    const auto s = f.logger->stats();
    balanced(s);
    require(s.written == 1 && s.rejected_stopped == 1 && s.pending == 0,
            "100 lifecycle rounds drained");
  }
  Fixture f(16, true);
  f.logger->submit(LogLevel::Info, "gate");
  require(f.gate.await_entry(), "stop sink entry");
  auto stop = std::async(std::launch::async, [&] { f.logger->stop(); });
  auto second_stop = std::async(std::launch::async, [&] { f.logger->stop(); });
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (f.logger->stats().rejected_stopped == 0 &&
         std::chrono::steady_clock::now() < deadline)
    f.logger->submit(LogLevel::Info, "race");
  const bool rejected = f.logger->stats().rejected_stopped != 0;
  const bool joining = stop.wait_for(20ms) == std::future_status::timeout;
  f.gate.release();
  stop.get();
  second_stop.get();
  balanced(f.logger->stats());
  require(rejected && joining && f.logger->stats().pending == 0,
          "stop rejects before release then joins");

  for (int mode = 0; mode < 2; ++mode) {
    bool failed = false;
    fail_allocation = mode == 0;
    fail_thread = mode == 1;
    try {
      hp::base::AsyncLogger logger;
    } catch (const std::exception&) {
      failed = true;
    }
    fail_allocation = false;
    fail_thread = false;
    require(failed, "startup failure consumed");
  }
  require(allocation_hits == 1 && thread_hits == 1,
          "allocation and thread injection exact hits");

  Gate capture;
  auto* old = std::clog.rdbuf(&capture);
  {
    hp::base::LoggerSession session;
    bool duplicate = false;
    try {
      hp::base::LoggerSession second;
    } catch (const std::logic_error&) {
      duplicate = true;
    }
    require(duplicate, "duplicate global session rejected");
    struct LastProducer {
      ~LastProducer() { hp::base::error("destructor before stop"); }
    };
    try {
      LastProducer last;
      throw std::runtime_error("fatal alive");
    } catch (const std::exception& error) {
      hp::base::error(error.what());
    }
    session.stop();
    hp::base::info("late no fallback");
    require(
        session.stats().written == 2 && session.stats().rejected_stopped == 1,
        "logger outlives producers and fatal catch");
  }
  hp::base::error("after session no fallback");
  std::clog.rdbuf(old);
  require(
      capture.text() == "[ERROR] destructor before stop\n[ERROR] fatal alive\n",
      "global fatal and destructor text");
  Gate concurrent_capture;
  old = std::clog.rdbuf(&concurrent_capture);
  auto session = std::make_unique<hp::base::LoggerSession>();
  hp::base::info("global accepted");
  std::barrier start(5);
  std::vector<std::thread> producers;
  for (int p = 0; p < 4; ++p) {
    producers.emplace_back([&] {
      start.arrive_and_wait();
      for (int i = 0; i < 1000; ++i) hp::base::info("global race");
    });
  }
  start.arrive_and_wait();
  session.reset();
  for (auto& producer : producers) producer.join();
  std::clog.rdbuf(old);
  require(
      concurrent_capture.writers().size() == 1 &&
          !concurrent_capture.writers().contains(std::this_thread::get_id()),
      "global destruction submission race never synchronously falls back");
  std::cout << "lifecycle 100 stop race startup hits=1/1 fatal late global "
               "race PASS\n";
}
}  // namespace

int main(int argc, char* argv[]) {
  try {
    const std::string mode = argc == 2 ? argv[1] : "all";
    if (mode == "all" || mode == "capacity") capacity_and_ownership();
    if (mode == "all" || mode == "concurrent") concurrent_and_failure();
    if (mode == "all" || mode == "lifecycle") lifecycle();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "async logger assertion: " << error.what() << '\n';
    return 1;
  }
}
