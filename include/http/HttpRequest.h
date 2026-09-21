#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace hp::http {

inline constexpr std::size_t kMaxRequestBytes = 16U * 1024U;
inline constexpr std::size_t kMaxRequestLineBytes = 4U * 1024U;

struct HttpRequest {
  std::string method;
  std::string target;
  bool closeRequested{false};
};

enum class ParseStatus {
  kNeedMore,
  kComplete,
  kBadRequest,
  kMethodNotAllowed,
};

struct ParseResult {
  ParseStatus status{ParseStatus::kNeedMore};
  HttpRequest request;
  std::size_t consumedBytes{0};
};

enum class ParserState { kRequestLine, kHeaders, kComplete, kError };

struct FeedResult {
  ParseStatus status{ParseStatus::kNeedMore};
  HttpRequest
      request;  // Independent value; safe after Reset or caller input release.
  std::size_t acceptedBytes{0};  // Bytes accepted from this feed only.
  std::size_t requestBytes{
      0};  // Cumulative, including the offending byte on error.
};

class RequestParser final {
 public:
  [[nodiscard]] FeedResult feed(std::string_view newBytes);
  void reset() noexcept;

  [[nodiscard]] ParserState state() const noexcept { return state_; }

  // Explicit byte visits in framing and completed-line validation loops.
  [[nodiscard]] std::size_t scanSteps() const noexcept { return scanSteps_; }

  [[nodiscard]] std::size_t bufferedBytes() const noexcept {
    return lineStart_ + lineSize_;
  }

  [[nodiscard]] std::size_t peakBufferedBytes() const noexcept {
    return peakBuffered_;
  }

  [[nodiscard]] bool pendingCr() const noexcept { return pendingCr_; }

 private:
  bool validateRequestLine();
  bool validateHeader();
  void finishLine();
  [[nodiscard]] FeedResult buildResult(std::size_t accepted) const;

  // Borrow input only for this call; each inspected character updates
  // scan_steps_.
  bool equalsAsciiCaseInsensitive(std::string_view value,
                                  std::string_view expected);
  std::string_view trimHeaderWhitespace(std::string_view value);

  // Request line retained once; subsequent Header lines reuse the rest. Never
  // retain a view into caller memory or copy a pipelined suffix into this
  // array.
  std::array<char, kMaxRequestBytes> storage_{};

  ParserState state_{ParserState::kRequestLine};
  ParseStatus status_{ParseStatus::kNeedMore};
  bool pendingCr_{false};
  bool hostSeen_{false};
  bool contentLengthSeen_{false};
  bool closeRequested_{false};

  std::size_t lineStart_{0}, lineSize_{0}, requestBytes_{0};
  std::size_t methodSize_{0}, targetStart_{0}, targetSize_{0};

  std::size_t scanSteps_{0}, peakBuffered_{0};
};

[[nodiscard]] ParseResult parseRequest(std::string_view bytes);

}  // namespace hp::http
