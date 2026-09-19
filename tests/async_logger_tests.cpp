#include <atomic>
#include <barrier>
#include <functional>
#include <future>
#include <iostream>
#include <vector>

#include "async_logger_faults.h"
#include "async_logger_test_support.h"
#include "base/logger.h"

namespace {
using namespace logger_test;
using hp::base::LogLevel;

void SubmitCapacityProbe(hp::base::AsyncLogger& logger,
                         const std::string& temporary) {
  logger.Submit(LogLevel::kWarn, temporary);
  logger.Submit(LogLevel::kError, "drop error");
  logger.Submit(LogLevel::kInfo, "drop info");
  logger.Submit(LogLevel::kWarn, "drop warn");
}

void SubmitOrderedRecords(hp::base::AsyncLogger& logger, int producer) {
  for (int i = 0; i < 1000; ++i) {
    while (logger.stats().pending >= 128) std::this_thread::yield();
    logger.Submit(LogLevel::kInfo,
                  std::to_string(producer) + ":" + std::to_string(i));
  }
}

void SubmitOverloadRecords(hp::base::AsyncLogger& logger, int producer) {
  for (int i = 0; i < 1000; ++i)
    logger.Submit(LogLevel::kError,
                  std::to_string(producer) + ":" + std::to_string(i));
}

void SubmitGlobalRaceRecords(std::barrier<>& start) {
  start.arrive_and_wait();
  for (int i = 0; i < 1000; ++i) hp::base::Info("global race");
}

void CapacityAndOwnership() {
  Gate gate;
  std::ostream sink(&gate);
  bool invalid = false;
  try {
    auto logger = hp::base::AsyncLoggerTestAccess::Make(0, sink);
  } catch (const std::invalid_argument&) {
    invalid = true;
  }
  Require(invalid, "capacity zero rejected");

  Fixture f(1, true);
  f.logger->Submit(LogLevel::kInfo, "inflight");
  Require(f.gate.AwaitEntry(), "sink entry handshake");
  std::string temporary = "owned temporary";
  auto producer = std::async(std::launch::async,
                             &SubmitCapacityProbe,
                             std::ref(*f.logger),
                             std::cref(temporary));
  const bool completed = producer.wait_for(200ms) == std::future_status::ready;
  f.gate.Release();
  producer.get();
  Require(completed, "producer must complete before sink release");
  // A separate blocked first record makes the borrowed-message mutation exact.
  f.logger->Stop();
  auto s = f.logger->stats();
  Balanced(s);
  Require(s.accepted == 2 && s.dropped_full == 3 && s.written == 2 &&
              s.pending == 0,
          "capacity one and all-level drop accounting");
  Require(f.gate.text() == "[INFO] inflight\n[WARN] owned temporary\n",
          "FIFO and compatible text");
  Require(f.gate.writers().size() == 1 &&
              !f.gate.writers().contains(std::this_thread::get_id()),
          "single consumer identity");

  Fixture owned(4, true);
  owned.logger->Submit(LogLevel::kInfo, "gate");
  Require(owned.gate.AwaitEntry(), "ownership gate entry");
  {
    std::string message = "original message";
    owned.logger->Submit(LogLevel::kInfo, message);
    message.assign(message.size(), 'X');
    // Keep the overwritten source alive until consumption, avoiding UB in the
    // narrow borrowed-message negative control.
    owned.gate.Release();
    owned.logger->Stop();
  }
  Require(owned.gate.text() == "[INFO] gate\n[INFO] original message\n",
          "owned message survives source overwrite");

  Fixture full_default(1024, true);
  full_default.logger->Submit(LogLevel::kInfo, "inflight");
  Require(full_default.gate.AwaitEntry(), "default capacity sink entry");
  for (int i = 0; i < 1024; ++i)
    full_default.logger->Submit(LogLevel::kInfo, "slot");
  full_default.logger->Submit(LogLevel::kError, "drop");
  const auto occupied = full_default.logger->stats();
  full_default.gate.Release();
  full_default.logger->Stop();
  Require(occupied.accepted == 1025 && occupied.pending == 1025 &&
              occupied.dropped_full == 1,
          "default capacity 1024 plus one inflight");
  Balanced(full_default.logger->stats());

  Fixture limits;
  limits.logger->Submit(LogLevel::kInfo, "");
  limits.logger->Submit(LogLevel::kInfo, std::string(1024, 'a'));
  limits.logger->Submit(LogLevel::kInfo, std::string(1025, 'b'));
  limits.logger->Stop();
  s = limits.logger->stats();
  Balanced(s);
  Require(s.accepted == 3 && s.truncated == 1 && s.written == 3,
          "message boundary accounting");
  Require(limits.gate.text() == "[INFO] \n[INFO] " + std::string(1024, 'a') +
                                    "\n[INFO] " + std::string(1010, 'b') +
                                    "...[truncated]\n",
          "message 0 1024 1025 exact truncation");
  std::cout << "capacity ownership boundaries PASS\n";
}

void ConcurrentAndFailure() {
  Fixture f;
  std::vector<std::thread> producers;
  for (int p = 0; p < 4; ++p) {
    producers.emplace_back(&SubmitOrderedRecords, std::ref(*f.logger), p);
  }
  for (auto& producer : producers) producer.join();
  f.logger->Stop();
  auto s = f.logger->stats();
  Balanced(s);
  Require(s.accepted == 4000 && s.written == 4000 && s.pending == 0 &&
              s.dropped_full == 0,
          "four producers 4000 no drop");
  int next[4]{};
  std::istringstream lines(f.gate.text());
  std::string line;
  while (std::getline(lines, line)) {
    int p = -1, i = -1;
    Require(std::sscanf(line.c_str(), "[INFO] %d:%d", &p, &i) == 2 && p >= 0 &&
                p < 4 && i == next[p]++,
            "per producer FIFO no duplicates");
  }
  for (auto value : next) Require(value == 1000, "no missing messages");
  Require(f.gate.flushes() == 4000, "each message flushed");

  Fixture overload(16, true);
  overload.logger->Submit(LogLevel::kInfo, "gate");
  Require(overload.gate.AwaitEntry(), "overload entered sink");
  producers.clear();
  for (int p = 0; p < 4; ++p) {
    producers.emplace_back(&SubmitOverloadRecords,
                           std::ref(*overload.logger),
                           p);
  }
  for (auto& producer : producers) producer.join();
  const auto saturated = overload.logger->stats();
  overload.gate.Release();
  overload.logger->Stop();
  Require(saturated.submitted == 4001 && saturated.accepted == 17 &&
              saturated.dropped_full == 3984 && saturated.pending == 17 &&
              overload.logger->stats().written == 17,
          "four producer overload exact accepted dropped pending");
  Balanced(overload.logger->stats());
  std::istringstream emitted(overload.gate.text());
  std::set<std::string> unique;
  while (std::getline(emitted, line))
    Require(unique.insert(line).second, "overload accepted messages unique");
  Require(unique.size() == 17, "overload accepted set matches count");

  for (int mode = 0; mode < 3; ++mode) {
    Fixture failure;
    failure.gate.fail_write_ = mode == 0;
    failure.gate.fail_flush_ = mode == 1;
    failure.gate.throw_write_ = mode == 2;
    if (mode == 2) failure.sink.exceptions(std::ios::badbit);
    for (int i = 0; i < 20; ++i)
      failure.logger->Submit(LogLevel::kError, "failure");
    failure.logger->Stop();
    s = failure.logger->stats();
    Balanced(s);
    Require(s.failed == 20 && s.written == 0 && s.pending == 0,
            "permanent write flush exception failure drained");
  }
  std::cout
      << "concurrency 4000 FIFO and write/flush/exception failures PASS\n";
}

void Lifecycle() {
  for (int i = 0; i < 100; ++i) {
    Fixture f;
    f.logger->Submit(LogLevel::kInfo, "cycle");
    f.logger->Stop();
    f.logger->Stop();
    f.logger->Submit(LogLevel::kError, "late");
    const auto s = f.logger->stats();
    Balanced(s);
    Require(s.written == 1 && s.rejected_stopped == 1 && s.pending == 0,
            "100 lifecycle rounds drained");
  }
  Fixture f(16, true);
  f.logger->Submit(LogLevel::kInfo, "gate");
  Require(f.gate.AwaitEntry(), "stop sink entry");
  auto stop = std::async(std::launch::async,
                         &hp::base::AsyncLogger::Stop,
                         f.logger.get());
  auto second_stop = std::async(std::launch::async,
                                &hp::base::AsyncLogger::Stop,
                                f.logger.get());
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (f.logger->stats().rejected_stopped == 0 &&
         std::chrono::steady_clock::now() < deadline)
    f.logger->Submit(LogLevel::kInfo, "race");
  const bool rejected = f.logger->stats().rejected_stopped != 0;
  const bool joining = stop.wait_for(20ms) == std::future_status::timeout;
  f.gate.Release();
  stop.get();
  second_stop.get();
  Balanced(f.logger->stats());
  Require(rejected && joining && f.logger->stats().pending == 0,
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
    Require(failed, "startup failure consumed");
  }
  Require(allocation_hits == 1 && thread_hits == 1,
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
    Require(duplicate, "duplicate global session rejected");
    struct LastProducer {
      ~LastProducer() { hp::base::Error("destructor before stop"); }
    };
    try {
      LastProducer last;
      throw std::runtime_error("fatal alive");
    } catch (const std::exception& error) {
      hp::base::Error(error.what());
    }
    session.Stop();
    hp::base::Info("late no fallback");
    Require(
        session.stats().written == 2 && session.stats().rejected_stopped == 1,
        "logger outlives producers and fatal catch");
  }
  hp::base::Error("after session no fallback");
  std::clog.rdbuf(old);
  Require(
      capture.text() == "[ERROR] destructor before stop\n[ERROR] fatal alive\n",
      "global fatal and destructor text");
  Gate concurrent_capture;
  old = std::clog.rdbuf(&concurrent_capture);
  auto session = std::make_unique<hp::base::LoggerSession>();
  hp::base::Info("global accepted");
  std::barrier start(5);
  std::vector<std::thread> producers;
  for (int p = 0; p < 4; ++p) {
    producers.emplace_back(&SubmitGlobalRaceRecords, std::ref(start));
  }
  start.arrive_and_wait();
  session.reset();
  for (auto& producer : producers) producer.join();
  std::clog.rdbuf(old);
  Require(
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
    if (mode == "all" || mode == "capacity") CapacityAndOwnership();
    if (mode == "all" || mode == "concurrent") ConcurrentAndFailure();
    if (mode == "all" || mode == "lifecycle") Lifecycle();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "async logger assertion: " << error.what() << '\n';
    return 1;
  }
}
