#include "base/logger.h"

#include <iostream>
#include <mutex>
#include <stdexcept>

#include "base/async_logger.h"

namespace hp::base {
namespace {

std::mutex output_mutex;
std::shared_ptr<AsyncLogger> current;
bool session_active = false;

void log(LogLevel level, std::string_view message) {
  std::shared_ptr<AsyncLogger> logger;
  {
    const std::lock_guard lock(output_mutex);
    logger = current;
    if (!logger) {
      const auto text = level == LogLevel::Info   ? "INFO"
                        : level == LogLevel::Warn ? "WARN"
                                                  : "ERROR";
      std::clog << '[' << text << "] " << message << '\n';
      return;
    }
  }
  // Shared ownership spans the complete submission, including concurrent stop.
  logger->submit(level, message);
}

}  // namespace

LoggerSession::LoggerSession() {
  const std::lock_guard lock(output_mutex);
  if (session_active) throw std::logic_error("logger session already active");
  logger_ = std::make_shared<AsyncLogger>();
  current = logger_;
  session_active = true;
}

LoggerSession::~LoggerSession() {
  stop();
  const std::lock_guard lock(output_mutex);
  session_active = false;
  // Retain the stopped instance for late submissions: never fall back to IO.
}

void LoggerSession::stop() { logger_->stop(); }

LogStats LoggerSession::stats() const { return logger_->stats(); }

void info(std::string_view message) { log(LogLevel::Info, message); }

void warn(std::string_view message) { log(LogLevel::Warn, message); }

void error(std::string_view message) { log(LogLevel::Error, message); }

}  // namespace hp::base
