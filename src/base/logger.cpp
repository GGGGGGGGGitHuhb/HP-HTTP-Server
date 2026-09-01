#include "base/logger.h"

#include <iostream>
#include <mutex>

namespace hp::base {
namespace {

std::mutex output_mutex;

void log(std::string_view level, std::string_view message) {
    const std::lock_guard lock(output_mutex);
    std::clog << '[' << level << "] " << message << '\n';
}

}  // namespace

void info(std::string_view message) {
    log("INFO", message);
}

void warn(std::string_view message) {
    log("WARN", message);
}

void error(std::string_view message) {
    log("ERROR", message);
}

}  // namespace hp::base
