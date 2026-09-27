#include "http/HttpRequest.h"

#include <algorithm>
#include <cctype>

namespace hp::http {
namespace {
bool isTokenCharacter(unsigned char c) {
  constexpr std::string_view kTokenPunctuation = "!#$%&'*+-.^_`|~";
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
         kTokenPunctuation.find(static_cast<char>(c)) != std::string_view::npos;
}
}  // namespace

bool RequestParser::validateRequestLine() {
  // 空格分割 method, target, version
  std::size_t first = lineSize_, second = lineSize_;
  for (std::size_t i = 0; i < lineSize_; ++i) {
    ++scanSteps_;
    if (storage_[i] != ' ') continue;
    if (first == lineSize_)
      first = i;
    else if (second == lineSize_)
      second = i;
    else
      return false;
  }
  if (first == 0 || second == lineSize_ || second <= first + 1) return false;
  for (std::size_t i = 0; i < first; ++i) {
    ++scanSteps_;
    if (!isTokenCharacter(static_cast<unsigned char>(storage_[i]))) return false;
  }
  if (storage_[first + 1] != '/') return false;
  for (std::size_t i = first + 1; i < second; ++i) {
    ++scanSteps_;
    const auto c = static_cast<unsigned char>(storage_[i]);
    if (c <= 0x20U || c == 0x7fU || c == '#') return false;
  }
  constexpr std::string_view kHttpVersion = "HTTP/1.1";
  if (lineSize_ - second - 1 != kHttpVersion.size()) return false;
  for (std::size_t i = 0; i < kHttpVersion.size(); ++i) {
    ++scanSteps_;
    if (storage_[second + 1 + i] != kHttpVersion[i]) return false;
  }
  methodSize_ = first;
  targetStart_ = first + 1;
  targetSize_ = second - first - 1;
  return true;
}

bool RequestParser::equalsAsciiCaseInsensitive(std::string_view value, std::string_view expected) {
  if (value.size() != expected.size()) return false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    ++scanSteps_;
    const char c = value[i] >= 'A' && value[i] <= 'Z' ? value[i] + ('a' - 'A') : value[i];
    if (c != expected[i]) return false;
  }
  return true;
}

std::string_view RequestParser::trimHeaderWhitespace(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    ++scanSteps_;
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    ++scanSteps_;
    value.remove_suffix(1);
  }
  return value;
}

bool RequestParser::validateHeader() {
  const std::string_view line(storage_.data() + lineStart_, lineSize_);
  if (line.front() == ' ' || line.front() == '\t') return false;
  std::size_t colon = 0;
  for (; colon < line.size(); ++colon) {
    ++scanSteps_;
    if (line[colon] == ':') break;
  }
  if (colon == 0 || colon == line.size()) return false;
  for (std::size_t i = 0; i < colon; ++i) {
    ++scanSteps_;
    if (!isTokenCharacter(static_cast<unsigned char>(line[i]))) return false;
  }
  bool nonemptyValue = false;
  for (std::size_t i = colon + 1; i < line.size(); ++i) {
    ++scanSteps_;
    const auto c = static_cast<unsigned char>(line[i]);
    if (c == 0x7fU || (c < 0x20U && c != '\t')) return false;
    if (c != ' ' && c != '\t') nonemptyValue = true;
  }
  // eg. Host: example.com
  // name = "Host", value = "example.com"
  const auto name = line.substr(0, colon);
  const auto value = trimHeaderWhitespace(line.substr(colon + 1));
  if (equalsAsciiCaseInsensitive(name, "host")) {
    // 若此前已有 Host 或值为空则校验失败
    if (hostSeen_ || !nonemptyValue) return false;
    hostSeen_ = true;
  } else if (equalsAsciiCaseInsensitive(name, "content-length")) {
    if (contentLengthSeen_ || value.empty()) return false;
    contentLengthSeen_ = true;
    // 只支持十进制零：不做算术运算，因此不会溢出。
    for (char c : value) {
      ++scanSteps_;
      if (c != '0') return false;
    }
  } else if (equalsAsciiCaseInsensitive(name, "transfer-encoding") ||
             equalsAsciiCaseInsensitive(name, "expect")) {
    return false;
  } else if (equalsAsciiCaseInsensitive(name, "connection")) {
    std::size_t start = 0;
    for (std::size_t i = 0; i <= value.size(); ++i) {
      ++scanSteps_;
      if (i != value.size() && value[i] != ',') continue;
      const auto token = trimHeaderWhitespace(value.substr(start, i - start));
      for (unsigned char c : token) {
        ++scanSteps_;
        if (!isTokenCharacter(c)) return false;
      }
      if (equalsAsciiCaseInsensitive(token, "close")) closeRequested_ = true;
      start = i + 1;
    }
  }
  return true;
}

void RequestParser::finishLine() {
  if (state_ == ParserState::kRequestLine) {
    if (!validateRequestLine()) {
      state_ = ParserState::kError;
      return;
    }
    // 让下一次检查从上一次处理完毕的位置开始
    lineStart_ = lineSize_;
    state_ = ParserState::kHeaders;
  } else if (lineSize_ == 0) {
    if (!hostSeen_) {
      state_ = ParserState::kError;
      return;
    }
    state_ = ParserState::kComplete;
    const std::string_view method(storage_.data(), methodSize_);
    status_ = method == "GET" ? ParseStatus::kComplete : ParseStatus::kMethodNotAllowed;
  } else if (!validateHeader()) {
    state_ = ParserState::kError;
  }
  lineSize_ = 0;
}

FeedResult RequestParser::buildFeedResult(std::size_t accepted) const {
  FeedResult result{status_, {}, accepted, requestBytes_};
  if (state_ == ParserState::kComplete) {
    result.request.closeRequested = closeRequested_;
    result.request.method.assign(storage_.data(), methodSize_);
    result.request.target.assign(storage_.data() + targetStart_, targetSize_);
  }
  return result;
}

FeedResult RequestParser::feedRequestBytes(std::string_view bytes) {
  std::size_t accepted = 0;
  // 若 finishLine() 将 state_ 改为 kComplete
  // 下轮循环便停止，不继续读取
  // kComplete, kError 是终态
  while (accepted < bytes.size() && state_ != ParserState::kComplete &&
         state_ != ParserState::kError) {
    const char c = bytes[accepted];
    ++accepted;       // 局部变量，每次调用从 0 开始
    ++requestBytes_;  // RequestParser 的成员，跨调用保存
    ++scanSteps_;
    if (pendingCr_) {
      pendingCr_ = false;
      if (c != '\n')
        state_ = ParserState::kError;
      else
        finishLine();  // 可能将 state_ 改为 kComplete
    } else if (c == '\r') {
      pendingCr_ = true;
    } else if (c == '\n' || c == '\0') {
      state_ = ParserState::kError;
    } else if (state_ == ParserState::kRequestLine && lineSize_ == kMaxRequestLineBytes) {
      state_ = ParserState::kError;
    } else {
      // 复制字符到 RequestParser 自己的数组 storage_
      storage_[lineStart_ + lineSize_++] = c;
      peakBuffered_ = std::max(peakBuffered_, bufferedBytes());
    }
    if (requestBytes_ == kMaxRequestBytes && state_ != ParserState::kComplete)
      state_ = ParserState::kError;
    if (state_ == ParserState::kError) status_ = ParseStatus::kBadRequest;
  }
  return buildFeedResult(accepted);
}

void RequestParser::resetRequestParser() noexcept {
  state_ = ParserState::kRequestLine;
  status_ = ParseStatus::kNeedMore;
  pendingCr_ = hostSeen_ = contentLengthSeen_ = closeRequested_ = false;
  lineStart_ = lineSize_ = requestBytes_ = 0;
  methodSize_ = targetStart_ = targetSize_ = 0;
  scanSteps_ = peakBuffered_ = 0;
}

ParseResult parseRequest(std::string_view bytes) {
  RequestParser parser;
  const auto parsed = parser.feedRequestBytes(bytes);
  return {parsed.status,
          parsed.request,
          parsed.status == ParseStatus::kComplete || parsed.status == ParseStatus::kMethodNotAllowed
              ? parsed.requestBytes
              : 0};
}
}  // namespace hp::http
