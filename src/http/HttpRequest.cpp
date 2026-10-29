#include "http/HttpRequest.h"

#include <algorithm>
#include <cctype>
#include <limits>

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
  headRequest_ = std::string_view(storage_.data(), first) == "HEAD";
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
  if (state_ == ParserState::kTrailers)
    return equalsAsciiCaseInsensitive(name, "x-checksum") ||
           equalsAsciiCaseInsensitive(name, "x-trace-id");
  if (equalsAsciiCaseInsensitive(name, "host")) {
    // 若此前已有 Host 或值为空则校验失败
    if (hostSeen_ || !nonemptyValue) return false;
    hostSeen_ = true;
  } else if (equalsAsciiCaseInsensitive(name, "content-length")) {
    if (contentLengthSeen_ || transferEncodingSeen_ || value.empty()) return false;
    contentLengthSeen_ = true;
    // 在乘加前检查溢出，合法超限值留到全部头校验完后拒绝。
    for (char c : value) {
      ++scanSteps_;
      if (c < '0' || c > '9') return false;
      const auto digit = static_cast<std::uint64_t>(c - '0');
      if (contentLength_ > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return false;
      contentLength_ = contentLength_ * 10 + digit;
    }
  } else if (equalsAsciiCaseInsensitive(name, "transfer-encoding")) {
    if (transferEncodingSeen_ || contentLengthSeen_) return false;
    transferEncodingSeen_ = true;
    if (!validateTokenList(value, false)) return false;
  } else if (equalsAsciiCaseInsensitive(name, "expect")) {
    if (expectSeen_ || value.empty()) return false;
    expectSeen_ = true;
    expectContinue_ = equalsAsciiCaseInsensitive(value, "100-continue");
  } else if (equalsAsciiCaseInsensitive(name, "trailer")) {
    if (trailerDeclarationSeen_) return false;
    trailerDeclarationSeen_ = true;
    if (!validateTokenList(value, true)) return false;
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

bool RequestParser::validateTokenList(std::string_view value, bool trailerDeclaration) {
  bool chunked = false, checksum = false, trace = false;
  std::size_t start = 0;
  for (std::size_t end = 0; end <= value.size(); ++end) {
    ++scanSteps_;
    if (end != value.size() && value[end] != ',') continue;
    const auto token = trimHeaderWhitespace(value.substr(start, end - start));
    if (token.empty()) return false;
    for (unsigned char c : token) {
      ++scanSteps_;
      if (!isTokenCharacter(c)) return false;
    }
    if (trailerDeclaration) {
      if (equalsAsciiCaseInsensitive(token, "x-checksum")) {
        if (checksum) return false;
        checksum = true;
      } else if (equalsAsciiCaseInsensitive(token, "x-trace-id")) {
        if (trace) return false;
        trace = true;
      } else
        return false;
    } else {
      if (chunked) return false;  // chunked 必须唯一且在最后。
      chunked = equalsAsciiCaseInsensitive(token, "chunked");
      if (!chunked) unsupportedCoding_ = true;
    }
    start = end + 1;
  }
  return trailerDeclaration || chunked;
}

void RequestParser::finishHeaders() {
  if (!hostSeen_ || (transferEncodingSeen_ && contentLengthSeen_) ||
      (trailerDeclarationSeen_ && !transferEncodingSeen_)) {
    state_ = ParserState::kError;
    return;
  }
  if (contentLength_ > kMaxBodyBytes)
    status_ = ParseStatus::kPayloadTooLarge;
  else if (unsupportedCoding_)
    status_ = ParseStatus::kNotImplemented;
  else if (expectSeen_ && !expectContinue_)
    status_ = ParseStatus::kExpectationFailed;
  if (status_ != ParseStatus::kNeedMore) {
    state_ = ParserState::kError;
    return;
  }
  if (transferEncodingSeen_)
    state_ = ParserState::kChunkSize;
  else if (contentLength_ != 0)
    state_ = ParserState::kBody;
  else
    finishRequest();
  if (expectContinue_ && state_ != ParserState::kComplete) status_ = ParseStatus::kHeadersComplete;
}

bool RequestParser::validateChunkExtensions(std::string_view line, std::size_t position) {
  while (position < line.size()) {
    while (position < line.size() && (line[position] == ' ' || line[position] == '\t')) ++position;
    if (position == line.size() || line[position++] != ';') return false;
    while (position < line.size() && (line[position] == ' ' || line[position] == '\t')) ++position;
    const auto nameStart = position;
    while (position < line.size() && isTokenCharacter(static_cast<unsigned char>(line[position])))
      ++position;
    if (position == nameStart) return false;
    const auto afterName = position;
    while (position < line.size() && (line[position] == ' ' || line[position] == '\t')) ++position;
    if (position == line.size()) return position == afterName;
    if (line[position] != '=') continue;
    ++position;
    while (position < line.size() && (line[position] == ' ' || line[position] == '\t')) ++position;
    if (position == line.size()) return false;
    if (line[position] == '"') {
      ++position;
      bool closed = false;
      while (position < line.size()) {
        const auto c = static_cast<unsigned char>(line[position++]);
        if (c == '"') {
          closed = true;
          break;
        }
        if (c == '\\') {
          if (position == line.size()) return false;
          const auto escaped = static_cast<unsigned char>(line[position++]);
          if (!(escaped == '\t' || (escaped >= 0x20 && escaped != 0x7f))) return false;
        } else if (!(c == '\t' || c == ' ' || c == 0x21 || (c >= 0x23 && c != 0x7f)))
          return false;
      }
      if (!closed) return false;
    } else {
      const auto start = position;
      while (position < line.size() && isTokenCharacter(static_cast<unsigned char>(line[position])))
        ++position;
      if (position == start) return false;
    }
  }
  return true;
}

bool RequestParser::validateChunkSize() {
  const std::string_view line(storage_.data() + lineStart_, lineSize_);
  scanSteps_ += line.size();
  std::uint64_t size = 0;
  std::size_t position = 0;
  for (; position < line.size(); ++position) {
    const char c = line[position];
    unsigned digit;
    if (c >= '0' && c <= '9')
      digit = c - '0';
    else if (c >= 'a' && c <= 'f')
      digit = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
      digit = c - 'A' + 10;
    else
      break;
    if (size > (std::numeric_limits<std::uint64_t>::max() - digit) / 16) return false;
    size = size * 16 + digit;
  }
  if (position == 0 || !validateChunkExtensions(line, position)) return false;
  if (size > kMaxBodyBytes - body_.size()) {
    status_ = ParseStatus::kPayloadTooLarge;
    return false;
  }
  chunkRemaining_ = static_cast<std::size_t>(size);
  state_ = size == 0 ? ParserState::kTrailers : ParserState::kChunkData;
  return true;
}

void RequestParser::feedChunkMetadata(char byte) {
  ++metadataBytes_;
  if (state_ == ParserState::kTrailers) ++trailerBytes_;
  if (state_ == ParserState::kChunkSize) ++chunkLineBytes_;
  if (metadataBytes_ > kMaxChunkMetadataBytes || trailerBytes_ > kMaxTrailerBytes ||
      chunkLineBytes_ > kMaxChunkLineBytes) {
    state_ = ParserState::kError;
    return;
  }
  if (state_ == ParserState::kChunkDataCrlf) {
    if (byte != (dataCrlfBytes_ == 0 ? '\r' : '\n')) {
      state_ = ParserState::kError;
      return;
    }
    if (++dataCrlfBytes_ == 2) {
      dataCrlfBytes_ = 0;
      chunkLineBytes_ = 0;
      state_ = ParserState::kChunkSize;
    }
    return;
  }
  if (pendingCr_) {
    pendingCr_ = false;
    if (byte != '\n') {
      state_ = ParserState::kError;
      return;
    }
    if (state_ == ParserState::kChunkSize) {
      if (!validateChunkSize()) state_ = ParserState::kError;
      chunkLineBytes_ = 0;
    } else if (lineSize_ == 0)
      finishRequest();
    else if (!validateHeader())
      state_ = ParserState::kError;
    lineSize_ = 0;
  } else if (byte == '\r')
    pendingCr_ = true;
  else if (byte == '\n' || byte == '\0' || lineStart_ + lineSize_ == storage_.size())
    state_ = ParserState::kError;
  else
    storage_[lineStart_ + lineSize_++] = byte;
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
    finishHeaders();
  } else if (!validateHeader()) {
    state_ = ParserState::kError;
  }
  lineSize_ = 0;
}

void RequestParser::finishRequest() {
  state_ = ParserState::kComplete;
  status_ = ParseStatus::kComplete;
}

std::size_t RequestParser::appendBody(std::string_view bytes) {
  const auto count =
      std::min(bytes.size(), static_cast<std::size_t>(contentLength_) - body_.size());
  body_.append(bytes.data(), count);
  requestBytes_ += count;
  scanSteps_ += count;
  if (body_.size() == contentLength_) finishRequest();
  return count;
}

FeedResult RequestParser::buildFeedResult(std::size_t accepted) const {
  FeedResult result{status_, {}, accepted, requestBytes_};
  if (state_ == ParserState::kComplete) {
    result.request.closeRequested = closeRequested_;
    result.request.body = body_;
    result.request.method.assign(storage_.data(), methodSize_);
    result.request.target.assign(storage_.data() + targetStart_, targetSize_);
  }
  if (status_ == ParseStatus::kHeadersComplete)
    result.head = RequestHead{std::string(storage_.data(), methodSize_),
                              std::string(storage_.data() + targetStart_, targetSize_),
                              closeRequested_};
  return result;
}

FeedResult RequestParser::feedRequestBytes(std::string_view bytes) {
  std::size_t accepted = 0;
  if (status_ == ParseStatus::kHeadersComplete) status_ = ParseStatus::kNeedMore;
  // 若 finishLine() 将 state_ 改为 kComplete
  // 下轮循环便停止，不继续读取
  // kComplete, kError 是终态
  while (accepted < bytes.size() && state_ != ParserState::kComplete &&
         state_ != ParserState::kError) {
    if (state_ == ParserState::kBody) {
      accepted += appendBody(bytes.substr(accepted));
      continue;
    }
    if (state_ == ParserState::kChunkData) {
      const auto count = std::min(chunkRemaining_, bytes.size() - accepted);
      body_.append(bytes.data() + accepted, count);
      chunkRemaining_ -= count;
      accepted += count;
      requestBytes_ += count;
      scanSteps_ += count;
      if (chunkRemaining_ == 0) state_ = ParserState::kChunkDataCrlf;
      continue;
    }
    if (state_ == ParserState::kChunkSize || state_ == ParserState::kChunkDataCrlf ||
        state_ == ParserState::kTrailers) {
      ++requestBytes_;
      ++scanSteps_;
      feedChunkMetadata(bytes[accepted++]);
      if (state_ == ParserState::kError && status_ == ParseStatus::kNeedMore)
        status_ = ParseStatus::kBadRequest;
      continue;
    }
    const char c = bytes[accepted];
    ++headerBytes_;
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
    if (headerBytes_ == kMaxHeaderBytes &&
        (state_ == ParserState::kRequestLine || state_ == ParserState::kHeaders))
      state_ = ParserState::kError;
    if (state_ == ParserState::kError &&
        (status_ == ParseStatus::kNeedMore || status_ == ParseStatus::kHeadersComplete))
      status_ = ParseStatus::kBadRequest;
    if (status_ == ParseStatus::kHeadersComplete) break;
  }
  return buildFeedResult(accepted);
}

void RequestParser::resetRequestParser() noexcept {
  state_ = ParserState::kRequestLine;
  status_ = ParseStatus::kNeedMore;
  transferEncodingSeen_ = unsupportedCoding_ = expectSeen_ = expectContinue_ =
      trailerDeclarationSeen_ = false;
  chunkRemaining_ = metadataBytes_ = trailerBytes_ = chunkLineBytes_ = 0;
  dataCrlfBytes_ = 0;
  headRequest_ = false;
  pendingCr_ = hostSeen_ = contentLengthSeen_ = closeRequested_ = false;
  lineStart_ = lineSize_ = requestBytes_ = headerBytes_ = 0;
  contentLength_ = 0;
  std::string{}.swap(body_);
  methodSize_ = targetStart_ = targetSize_ = 0;
  scanSteps_ = peakBuffered_ = 0;
}

ParseResult parseRequest(std::string_view bytes) {
  RequestParser parser;
  auto parsed = parser.feedRequestBytes(bytes);
  if (parsed.status == ParseStatus::kHeadersComplete)
    parsed = parser.feedRequestBytes(bytes.substr(parsed.acceptedBytes));
  return {parsed.status,
          parsed.request,
          parsed.status == ParseStatus::kComplete ? parsed.requestBytes : 0};
}
}  // namespace hp::http
