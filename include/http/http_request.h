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
  bool close_requested{false};
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
  std::size_t consumed_bytes{0};
};

enum class ParserState { kRequestLine, kHeaders, kComplete, kError };

struct FeedResult {
  ParseStatus status{ParseStatus::kNeedMore};
  HttpRequest
      request;  // Independent value; safe after Reset or caller input release.
  std::size_t accepted_bytes{0};  // Bytes accepted from this feed only.
  std::size_t request_bytes{
      0};  // Cumulative, including the offending byte on error.
};

class RequestParser final {
 public:
  [[nodiscard]] FeedResult Feed(std::string_view new_bytes);
  void Reset() noexcept;

  [[nodiscard]] ParserState state() const noexcept { return state_; }

  // Explicit byte visits in framing and completed-line validation loops.
  [[nodiscard]] std::size_t scan_steps() const noexcept { return scan_steps_; }

  [[nodiscard]] std::size_t buffered_bytes() const noexcept {
    return line_start_ + line_size_;
  }

  [[nodiscard]] std::size_t peak_buffered_bytes() const noexcept {
    return peak_buffered_;
  }

  [[nodiscard]] bool pending_cr() const noexcept { return pending_cr_; }

 private:
  bool ValidateRequestLine();
  bool ValidateHeader();
  void FinishLine();
  [[nodiscard]] FeedResult BuildResult(std::size_t accepted) const;

  // Borrow input only for this call; each inspected character updates
  // scan_steps_.
  bool EqualsAsciiCaseInsensitive(std::string_view value,
                                  std::string_view expected);
  std::string_view TrimHeaderWhitespace(std::string_view value);

  // Request line retained once; subsequent Header lines reuse the rest. Never
  // retain a view into caller memory or copy a pipelined suffix into this
  // array.
  std::array<char, kMaxRequestBytes> storage_{};

  ParserState state_{ParserState::kRequestLine};
  ParseStatus status_{ParseStatus::kNeedMore};
  bool pending_cr_{false};
  bool host_seen_{false};
  bool content_length_seen_{false};
  bool close_requested_{false};

  std::size_t line_start_{0}, line_size_{0}, request_bytes_{0};
  std::size_t method_size_{0}, target_start_{0}, target_size_{0};

  std::size_t scan_steps_{0}, peak_buffered_{0};
};

[[nodiscard]] ParseResult ParseRequest(std::string_view bytes);

}  // namespace hp::http
