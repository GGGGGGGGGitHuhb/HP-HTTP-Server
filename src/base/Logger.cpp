#include "base/Logger.h"

#include <iostream>
#include <mutex>
#include <stdexcept>

#include "base/AsyncLogger.h"

namespace hp::base {
namespace {

std::mutex outputMutex;
std::shared_ptr<AsyncLogger> activeLogger;
bool sessionActive = false;

void writeLog(LogLevel level, std::string_view message) {
  std::shared_ptr<AsyncLogger> logger;
  {
    const std::lock_guard lock(outputMutex);
    logger = activeLogger;
    if (!logger) {
      const auto text = level == LogLevel::kInfo   ? "INFO"
                        : level == LogLevel::kWarn ? "WARN"
                                                   : "ERROR";
      std::clog << '[' << text << "] " << message << '\n';
      return;
    }
  }
  // 共享所有权覆盖完整提交过程，包括并发 Stop。
  logger->submitLogRecord(level, message);
}

}  // namespace

LoggerSession::LoggerSession() {
  const std::lock_guard lock(outputMutex);
  if (sessionActive) throw std::logic_error("logger session already active");
  logger_ = std::make_shared<AsyncLogger>();
  activeLogger = logger_;
  sessionActive = true;
}

LoggerSession::~LoggerSession() {
  stopSessionLogging();
  const std::lock_guard lock(outputMutex);
  sessionActive = false;
  // 保留已停止实例以接收迟到的提交：不得回退到 IO。
}

void LoggerSession::stopSessionLogging() { logger_->stopAsyncLogging(); }

LogStats LoggerSession::stats() const { return logger_->stats(); }

void info(std::string_view message) { writeLog(LogLevel::kInfo, message); }

void warn(std::string_view message) { writeLog(LogLevel::kWarn, message); }

void error(std::string_view message) { writeLog(LogLevel::kError, message); }

}  // namespace hp::base
