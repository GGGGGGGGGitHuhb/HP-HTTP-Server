#include "AccessLog.h"

#include <algorithm>
#include <stdexcept>

#include "base/AsyncLogger.h"
#include "base/Logger.h"

namespace hp::app {
namespace {
// 固定字段与四个20位数字最多240字节，最坏转义672字节，总计<=912。
static_assert(6 * (AccessRecord::kMethodLimit + AccessRecord::kPathLimit) + 240 <=
              base::AsyncLogger::kMessageLimit);

void appendJsonString(std::string& output, std::string_view input) {
  constexpr char kHex[] = "0123456789abcdef";
  output += '"';
  for (unsigned char byte : input) {
    if (byte == '"' || byte == '\\') {
      output += '\\';
      output += static_cast<char>(byte);
    } else if (byte < 0x20 || byte >= 0x7f) {
      output += "\\u00";
      output += kHex[byte >> 4];
      output += kHex[byte & 15];
    } else {
      output += static_cast<char>(byte);
    }
  }
  output += '"';
}
}  // namespace

void AccessRecord::copyRequest(std::string_view requestMethod, std::string_view target) noexcept {
  methodLength = std::min(requestMethod.size(), method.size());
  std::copy_n(requestMethod.begin(), methodLength, method.begin());
  const auto pathOnly = target.substr(0, target.find_first_of("?#"));
  pathLength = std::min(pathOnly.size(), path.size());
  std::copy_n(pathOnly.begin(), pathLength, path.begin());
  pathTruncated = pathOnly.size() > path.size();
}

std::string serializeAccessRecord(const AccessRecord& record) {
  std::string output = "{\"event\":\"http_access\",\"method\":";
  output.reserve(base::AsyncLogger::kMessageLimit);
  appendJsonString(output, {record.method.data(), record.methodLength});
  output += ",\"path\":";
  appendJsonString(output, {record.path.data(), record.pathLength});
  output += ",\"status\":" + std::to_string(record.status);
  output += ",\"content_bytes\":" + std::to_string(record.contentBytes);
  output += ",\"duration_us\":" + std::to_string(record.durationUs);
  output += record.aborted ? ",\"outcome\":\"aborted\"" : ",\"outcome\":\"completed\"";
  output += record.pathTruncated ? ",\"path_truncated\":true}" : ",\"path_truncated\":false}";
  if (output.size() > base::AsyncLogger::kMessageLimit)
    throw std::length_error("access record limit");
  return output;
}

void submitAccessRecord(const AccessRecord& record, metrics::ServerMetrics* metrics) noexcept {
  try {
    base::info(serializeAccessRecord(record));
  } catch (...) {
    if (metrics) metrics->recordAccessLogFailure();
  }
}
}  // namespace hp::app
