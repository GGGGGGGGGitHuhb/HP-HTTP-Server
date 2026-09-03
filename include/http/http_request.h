#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace hp::http {

inline constexpr std::size_t max_request_bytes = 16U * 1024U;
inline constexpr std::size_t max_request_line_bytes = 4U * 1024U;

struct HttpRequest {
    std::string method;
    std::string target;
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

[[nodiscard]] ParseResult parse_request(std::string_view bytes);

}  // namespace hp::http
