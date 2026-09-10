#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace hp::http {

inline constexpr std::size_t max_request_bytes = 16U * 1024U;
inline constexpr std::size_t max_request_line_bytes = 4U * 1024U;

struct HttpRequest {
  std::string method;
  std::string target;
  bool close_requested{false};
};

enum class ParseStatus {
  need_more,
  complete,
  bad_request,
  method_not_allowed,
};

struct ParseResult {
  ParseStatus status{ParseStatus::need_more};
  HttpRequest request;
  std::size_t consumed_bytes{0};
};

enum class ParserState { request_line, headers, complete, error };

struct FeedResult {
  ParseStatus status{ParseStatus::need_more};
  HttpRequest
      request;  // Independent value; safe after reset or caller input release.
  std::size_t accepted_bytes{0};  // Bytes accepted from this feed only.
  std::size_t request_bytes{
      0};  // Cumulative, including the offending byte on error.
};

class RequestParser final {
 public:
  [[nodiscard]] FeedResult feed(std::string_view new_bytes);
  void reset() noexcept;

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
  bool validate_request_line();
  bool validate_header();
  void finish_line();
  [[nodiscard]] FeedResult result(std::size_t accepted) const;

  // Request line retained once; subsequent Header lines reuse the rest. Never
  // retain a view into caller memory or copy a pipelined suffix into this
  // array.
  std::array<char, max_request_bytes> storage_{};

  ParserState state_{ParserState::request_line};
  ParseStatus status_{ParseStatus::need_more};
  bool pending_cr_{false};
  bool host_seen_{false};
  bool content_length_seen_{false};
  bool close_requested_{false};

  std::size_t line_start_{0}, line_size_{0}, request_bytes_{0};
  std::size_t method_size_{0}, target_start_{0}, target_size_{0};

  std::size_t scan_steps_{0}, peak_buffered_{0};
};

[[nodiscard]] ParseResult parse_request(std::string_view bytes);

}  // namespace hp::http
