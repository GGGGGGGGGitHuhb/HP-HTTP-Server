#include "base/logger.h"

#include <iostream>
#include <mutex>
#include <stdexcept>

#include "base/async_logger.h"

namespace hp::base {
namespace {

std::mutex output_mutex;
std::shared_ptr<AsyncLogger> active_logger;
bool session_active = false;

void WriteLog(LogLevel level, std::string_view message) {
  std::shared_ptr<AsyncLogger> logger;
  {
    const std::lock_guard lock(output_mutex);
    logger = active_logger;
    if (!logger) {
      const auto text = level == LogLevel::kInfo   ? "INFO"
                        : level == LogLevel::kWarn ? "WARN"
                                                   : "ERROR";
      std::clog << '[' << text << "] " << message << '\n';
      return;
    }
  }
  // Shared ownership spans the complete submission, including concurrent Stop.
  logger->Submit(level, message);
}

}  // namespace

LoggerSession::LoggerSession() {
  const std::lock_guard lock(output_mutex);
  if (session_active) throw std::logic_error("logger session already active");
  logger_ = std::make_shared<AsyncLogger>();
  active_logger = logger_;
  session_active = true;
}

LoggerSession::~LoggerSession() {
  Stop();
  const std::lock_guard lock(output_mutex);
  session_active = false;
  // Retain the stopped instance for late submissions: never fall back to IO.
}

void LoggerSession::Stop() { logger_->Stop(); }

LogStats LoggerSession::stats() const { return logger_->stats(); }

void Info(std::string_view message) { WriteLog(LogLevel::kInfo, message); }

void Warn(std::string_view message) { WriteLog(LogLevel::kWarn, message); }

void Error(std::string_view message) { WriteLog(LogLevel::kError, message); }

}  // namespace hp::base
