#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

#include "base/AsyncLogger.h"

namespace hp::base {

struct AsyncLoggerTestAccess {
  static std::unique_ptr<AsyncLogger> createLogger(std::size_t slots, std::ostream& sink) {
    return std::unique_ptr<AsyncLogger>(new AsyncLogger(slots, sink));
  }

  static bool isAccepting(const AsyncLogger& logger) {
    const std::lock_guard lock(logger.mutex_);
    return logger.accepting_;
  }
};

}  // namespace hp::base

namespace {

using hp::base::AsyncLogger;
using hp::base::AsyncLoggerTestAccess;
using hp::base::LogLevel;
using hp::base::LogStats;
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void requireConserved(const LogStats& stats) {
  require(stats.submitted == stats.accepted + stats.droppedFull + stats.rejectedStopped,
          "提交计数守恒");
  require(stats.accepted == stats.written + stats.failed + stats.pending, "接受计数守恒");
}

enum class SinkFailure { kNone, kShortWrite, kWriteException, kFlushFailure, kFlushException };

class ControlledSink : public std::streambuf {
 public:
  explicit ControlledSink(SinkFailure failure = SinkFailure::kNone) : failure_(failure) {}

  void waitForFirstWrite() {
    std::unique_lock lock(mutex_);
    require(ready_.wait_for(lock, 5s, [this] { return firstWriteEntered(); }),
            "第一条日志及时到达sink，无需凑批");
  }

  void releaseFirstWrite() {
    const std::lock_guard lock(mutex_);
    released_ = true;
    ready_.notify_all();
  }

  std::string output() const {
    const std::lock_guard lock(mutex_);
    return output_;
  }

  std::vector<std::string> batches() const {
    const std::lock_guard lock(mutex_);
    return batches_;
  }

  std::size_t flushCount() const {
    const std::lock_guard lock(mutex_);
    return flushCount_;
  }

 protected:
  std::streamsize xsputn(const char* data, std::streamsize length) override {
    std::unique_lock lock(mutex_);
    ++writeCount_;
    if (writeCount_ == 1) {
      entered_ = true;
      ready_.notify_all();
      ready_.wait(lock, [this] { return firstWriteReleased(); });
    }
    if (writeCount_ == 2 && failure_ == SinkFailure::kWriteException)
      throw std::runtime_error("受控写入异常");
    if (writeCount_ == 2 && failure_ == SinkFailure::kShortWrite) {
      output_.append(data, length / 2);
      return length / 2;
    }
    batches_.emplace_back(data, length);
    output_.append(data, length);
    return length;
  }

  int sync() override {
    const std::lock_guard lock(mutex_);
    ++flushCount_;
    if (flushCount_ == 2 && failure_ == SinkFailure::kFlushException)
      throw std::runtime_error("受控flush异常");
    return flushCount_ == 2 && failure_ == SinkFailure::kFlushFailure ? -1 : 0;
  }

 private:
  bool firstWriteEntered() const { return entered_; }
  bool firstWriteReleased() const { return released_; }

  SinkFailure failure_;
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  bool entered_ = false;
  bool released_ = false;
  std::size_t writeCount_ = 0;
  std::size_t flushCount_ = 0;
  std::string output_;
  std::vector<std::string> batches_;
};

struct LoggerFixture {
  explicit LoggerFixture(std::size_t slots = AsyncLogger::kCapacity,
                         SinkFailure failure = SinkFailure::kNone)
      : sink(failure), stream(&sink), logger(AsyncLoggerTestAccess::createLogger(slots, stream)) {
    stream.exceptions(std::ios::badbit | std::ios::failbit);
  }

  ~LoggerFixture() {
    sink.releaseFirstWrite();
    logger->stopAsyncLogging();
  }

  void blockFirstRecord() {
    logger->submitLogRecord(LogLevel::kInfo, "gate");
    sink.waitForFirstWrite();
  }

  ControlledSink sink;
  std::ostream stream;
  std::unique_ptr<AsyncLogger> logger;
};

std::string logLine(LogLevel level, const std::string& message) {
  const char* prefix = level == LogLevel::kInfo   ? "[INFO] "
                       : level == LogLevel::kWarn ? "[WARN] "
                                                  : "[ERROR] ";
  std::string body = message.substr(0, AsyncLogger::kMessageLimit);
  if (message.size() > AsyncLogger::kMessageLimit) {
    const std::string marker = "...[truncated]";
    body.replace(body.size() - marker.size(), marker.size(), marker);
  }
  return prefix + body + '\n';
}

void checkBatchBoundaryAndContents() {
  LoggerFixture fixture;
  fixture.blockFirstRecord();
  std::string expected = "[INFO] gate\n";
  for (int index = 0; index < 65; ++index) {
    const std::string message = std::to_string(index);
    fixture.logger->submitLogRecord(LogLevel::kInfo, message);
    expected += logLine(LogLevel::kInfo, message);
  }
  const std::vector<std::pair<LogLevel, std::string>> extra = {
      {LogLevel::kInfo, ""},
      {LogLevel::kWarn, std::string("a\0b", 3)},
      {LogLevel::kError, std::string(1024, 'x')},
      {LogLevel::kWarn, std::string(1100, 'y')}};
  for (const auto& [level, message] : extra) {
    fixture.logger->submitLogRecord(level, message);
    expected += logLine(level, message);
  }
  const auto inFlight = fixture.logger->stats();
  requireConserved(inFlight);
  require(inFlight.accepted == 70 && inFlight.pending == 70, "pending包含队列与在途记录");
  fixture.sink.releaseFirstWrite();
  fixture.logger->stopAsyncLogging();
  const auto stats = fixture.logger->stats();
  requireConserved(stats);
  require(stats.written == 70 && stats.pending == 0 && stats.truncated == 1,
          "所有已接受记录排空，截断计数保持");
  require(fixture.sink.output() == expected, "等级/空正文/NUL/截断/FIFO逐字保持");
  const auto batches = fixture.sink.batches();
  require(batches.size() == 3 && fixture.sink.flushCount() == 3, "一次sink写入与flush对应一个批次");
  require(std::count(batches[1].begin(), batches[1].end(), '\n') == 64, "满批次64条");
  require(std::count(batches[2].begin(), batches[2].end(), '\n') == 5, "末尾不足64条也排空");
}

void checkCapacityAndStoppedSubmission() {
  LoggerFixture fixture(2);
  fixture.blockFirstRecord();
  fixture.logger->submitLogRecord(LogLevel::kWarn, "queued1");
  fixture.logger->submitLogRecord(LogLevel::kError, "queued2");
  fixture.logger->submitLogRecord(LogLevel::kError, "dropped");
  const auto before = fixture.logger->stats();
  requireConserved(before);
  require(before.accepted == 3 && before.pending == 3 && before.droppedFull == 1,
          "在途记录之外，满队列全部等级丢新");
  fixture.sink.releaseFirstWrite();
  fixture.logger->stopAsyncLogging();
  fixture.logger->submitLogRecord(LogLevel::kError, "late");
  const auto after = fixture.logger->stats();
  requireConserved(after);
  require(after.written == 3 && after.rejectedStopped == 1 && after.pending == 0,
          "停机排空，迟到提交仅拒绝");
  require(fixture.sink.output() == "[INFO] gate\n[WARN] queued1\n[ERROR] queued2\n",
          "无ERROR同步回退");
}

void submitProducerRecords(AsyncLogger* logger, int producer) {
  for (int index = 0; index < 200; ++index)
    logger->submitLogRecord(LogLevel::kInfo,
                            std::to_string(producer) + ':' + std::to_string(index));
}

void checkConcurrentProducersAndStops() {
  LoggerFixture fixture;
  fixture.blockFirstRecord();
  std::vector<std::thread> producers;
  for (int producer = 0; producer < 4; ++producer)
    producers.emplace_back(submitProducerRecords, fixture.logger.get(), producer);
  for (auto& producer : producers) producer.join();
  require(fixture.logger->stats().accepted == 801, "四生产者无丢失");

  std::thread firstStop(&AsyncLogger::stopAsyncLogging, fixture.logger.get());
  std::thread secondStop(&AsyncLogger::stopAsyncLogging, fixture.logger.get());
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (AsyncLoggerTestAccess::isAccepting(*fixture.logger) &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  const bool stopped = !AsyncLoggerTestAccess::isAccepting(*fixture.logger);
  fixture.logger->submitLogRecord(LogLevel::kError, "late");
  fixture.sink.releaseFirstWrite();
  firstStop.join();
  secondStop.join();
  require(stopped, "并发stop及时禁止提交");
  const auto stats = fixture.logger->stats();
  requireConserved(stats);
  require(stats.written == 801 && stats.pending == 0 && stats.rejectedStopped == 1,
          "并发stop只join一次并排空");
  require(fixture.sink.batches().size() == 14 && fixture.sink.flushCount() == 14,
          "并发队列按64条上限输出");

  std::istringstream lines(fixture.sink.output());
  std::string line;
  std::getline(lines, line);
  require(line == "[INFO] gate", "首条先于所有生产者");
  std::array<int, 4> nextIndex{};
  while (std::getline(lines, line)) {
    require(line.starts_with("[INFO] "), "生产者等级保持");
    const auto colon = line.find(':');
    const int producer = std::stoi(line.substr(7, colon - 7));
    const int index = std::stoi(line.substr(colon + 1));
    require(producer >= 0 && producer < 4, "生产者身份合法");
    require(index == nextIndex[producer]++, "每个生产者FIFO及无重复");
  }
  for (int count : nextIndex) require(count == 200, "每生产者200条完整输出");
}

void checkBatchFailure(SinkFailure failure, bool throws) {
  LoggerFixture fixture(AsyncLogger::kCapacity, failure);
  if (!throws) fixture.stream.exceptions(std::ios::goodbit);
  fixture.blockFirstRecord();
  for (int index = 0; index < 66; ++index)
    fixture.logger->submitLogRecord(LogLevel::kError, std::to_string(index));
  fixture.sink.releaseFirstWrite();
  fixture.logger->stopAsyncLogging();
  const auto stats = fixture.logger->stats();
  requireConserved(stats);
  require(stats.accepted == 67 && stats.written == 1 && stats.failed == 66 && stats.pending == 0,
          "失败批次整批计failed，后续失败排空，不重试");
  const bool flushFailure =
      failure == SinkFailure::kFlushFailure || failure == SinkFailure::kFlushException;
  require(fixture.sink.flushCount() == (flushFailure ? 2 : 1), "永久失败sink不重复flush");
}

}  // namespace

int main() {
  try {
    checkBatchBoundaryAndContents();
    checkCapacityAndStoppedSubmission();
    checkConcurrentProducersAndStops();
    checkBatchFailure(SinkFailure::kShortWrite, false);
    checkBatchFailure(SinkFailure::kShortWrite, true);
    checkBatchFailure(SinkFailure::kWriteException, true);
    checkBatchFailure(SinkFailure::kFlushFailure, false);
    checkBatchFailure(SinkFailure::kFlushFailure, true);
    checkBatchFailure(SinkFailure::kFlushException, true);
    std::cout << "AsyncLoggerBatch: 9 cases PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
