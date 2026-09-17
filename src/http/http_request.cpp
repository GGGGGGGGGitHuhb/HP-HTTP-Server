#include "http/http_request.h"

#include <algorithm>
#include <cctype>

namespace hp::http {
namespace {
bool IsTokenCharacter(unsigned char c) {
  constexpr std::string_view kTokenPunctuation = "!#$%&'*+-.^_`|~";
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') ||
         kTokenPunctuation.find(static_cast<char>(c)) != std::string_view::npos;
}
}  // namespace

bool RequestParser::ValidateRequestLine() {
  std::size_t first = line_size_, second = line_size_;
  for (std::size_t i = 0; i < line_size_; ++i) {
    ++scan_steps_;
    if (storage_[i] != ' ') continue;
    if (first == line_size_)
      first = i;
    else if (second == line_size_)
      second = i;
    else
      return false;
  }
  if (first == 0 || second == line_size_ || second <= first + 1) return false;
  for (std::size_t i = 0; i < first; ++i) {
    ++scan_steps_;
    if (!IsTokenCharacter(static_cast<unsigned char>(storage_[i])))
      return false;
  }
  if (storage_[first + 1] != '/') return false;
  for (std::size_t i = first + 1; i < second; ++i) {
    ++scan_steps_;
    const auto c = static_cast<unsigned char>(storage_[i]);
    if (c <= 0x20U || c == 0x7fU || c == '#') return false;
  }
  constexpr std::string_view kHttpVersion = "HTTP/1.1";
  if (line_size_ - second - 1 != kHttpVersion.size()) return false;
  for (std::size_t i = 0; i < kHttpVersion.size(); ++i) {
    ++scan_steps_;
    if (storage_[second + 1 + i] != kHttpVersion[i]) return false;
  }
  method_size_ = first;
  target_start_ = first + 1;
  target_size_ = second - first - 1;
  return true;
}

bool RequestParser::EqualsAsciiCaseInsensitive(std::string_view value,
                                               std::string_view expected) {
  if (value.size() != expected.size()) return false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    ++scan_steps_;
    const char c =
        value[i] >= 'A' && value[i] <= 'Z' ? value[i] + ('a' - 'A') : value[i];
    if (c != expected[i]) return false;
  }
  return true;
}

std::string_view RequestParser::TrimHeaderWhitespace(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    ++scan_steps_;
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    ++scan_steps_;
    value.remove_suffix(1);
  }
  return value;
}

bool RequestParser::ValidateHeader() {
  const std::string_view line(storage_.data() + line_start_, line_size_);
  if (line.front() == ' ' || line.front() == '\t') return false;
  std::size_t colon = 0;
  for (; colon < line.size(); ++colon) {
    ++scan_steps_;
    if (line[colon] == ':') break;
  }
  if (colon == 0 || colon == line.size()) return false;
  for (std::size_t i = 0; i < colon; ++i) {
    ++scan_steps_;
    if (!IsTokenCharacter(static_cast<unsigned char>(line[i]))) return false;
  }
  bool nonempty_value = false;
  for (std::size_t i = colon + 1; i < line.size(); ++i) {
    ++scan_steps_;
    const auto c = static_cast<unsigned char>(line[i]);
    if (c == 0x7fU || (c < 0x20U && c != '\t')) return false;
    if (c != ' ' && c != '\t') nonempty_value = true;
  }
  const auto name = line.substr(0, colon);
  const auto value = TrimHeaderWhitespace(line.substr(colon + 1));
  if (EqualsAsciiCaseInsensitive(name, "host")) {
    if (host_seen_ || !nonempty_value) return false;
    host_seen_ = true;
  } else if (EqualsAsciiCaseInsensitive(name, "content-length")) {
    if (content_length_seen_ || value.empty()) return false;
    content_length_seen_ = true;
    // Only decimal zero is supported: no arithmetic and therefore no overflow.
    for (char c : value) {
      ++scan_steps_;
      if (c != '0') return false;
    }
  } else if (EqualsAsciiCaseInsensitive(name, "transfer-encoding") ||
             EqualsAsciiCaseInsensitive(name, "expect")) {
    return false;
  } else if (EqualsAsciiCaseInsensitive(name, "connection")) {
    std::size_t start = 0;
    for (std::size_t i = 0; i <= value.size(); ++i) {
      ++scan_steps_;
      if (i != value.size() && value[i] != ',') continue;
      const auto token = TrimHeaderWhitespace(value.substr(start, i - start));
      for (unsigned char c : token) {
        ++scan_steps_;
        if (!IsTokenCharacter(c)) return false;
      }
      if (EqualsAsciiCaseInsensitive(token, "close")) close_requested_ = true;
      start = i + 1;
    }
  }
  return true;
}

void RequestParser::FinishLine() {
  if (state_ == ParserState::kRequestLine) {
    if (!ValidateRequestLine()) {
      state_ = ParserState::kError;
      return;
    }
    line_start_ = line_size_;
    state_ = ParserState::kHeaders;
  } else if (line_size_ == 0) {
    if (!host_seen_) {
      state_ = ParserState::kError;
      return;
    }
    state_ = ParserState::kComplete;
    const std::string_view method(storage_.data(), method_size_);
    status_ = method == "GET" ? ParseStatus::kComplete
                              : ParseStatus::kMethodNotAllowed;
  } else if (!ValidateHeader()) {
    state_ = ParserState::kError;
  }
  line_size_ = 0;
}

FeedResult RequestParser::BuildResult(std::size_t accepted) const {
  FeedResult result{status_, {}, accepted, request_bytes_};
  if (state_ == ParserState::kComplete) {
    result.request.close_requested = close_requested_;
    result.request.method.assign(storage_.data(), method_size_);
    result.request.target.assign(storage_.data() + target_start_, target_size_);
  }
  return result;
}

FeedResult RequestParser::Feed(std::string_view bytes) {
  std::size_t accepted = 0;
  while (accepted < bytes.size() && state_ != ParserState::kComplete &&
         state_ != ParserState::kError) {
    const char c = bytes[accepted];
    ++accepted;
    ++request_bytes_;
    ++scan_steps_;
    if (pending_cr_) {
      pending_cr_ = false;
      if (c != '\n')
        state_ = ParserState::kError;
      else
        FinishLine();
    } else if (c == '\r') {
      pending_cr_ = true;
    } else if (c == '\n' || c == '\0') {
      state_ = ParserState::kError;
    } else if (state_ == ParserState::kRequestLine &&
               line_size_ == kMaxRequestLineBytes) {
      state_ = ParserState::kError;
    } else {
      storage_[line_start_ + line_size_++] = c;
      peak_buffered_ = std::max(peak_buffered_, buffered_bytes());
    }
    if (request_bytes_ == kMaxRequestBytes && state_ != ParserState::kComplete)
      state_ = ParserState::kError;
    if (state_ == ParserState::kError) status_ = ParseStatus::kBadRequest;
  }
  return BuildResult(accepted);
}

void RequestParser::Reset() noexcept {
  state_ = ParserState::kRequestLine;
  status_ = ParseStatus::kNeedMore;
  pending_cr_ = host_seen_ = content_length_seen_ = close_requested_ = false;
  line_start_ = line_size_ = request_bytes_ = 0;
  method_size_ = target_start_ = target_size_ = 0;
  scan_steps_ = peak_buffered_ = 0;
}

ParseResult ParseRequest(std::string_view bytes) {
  RequestParser parser;
  const auto parsed = parser.Feed(bytes);
  return {parsed.status,
          parsed.request,
          parsed.status == ParseStatus::kComplete ||
                  parsed.status == ParseStatus::kMethodNotAllowed
              ? parsed.request_bytes
              : 0};
}
}  // namespace hp::http
