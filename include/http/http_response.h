#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hp::http {

enum class Status {
    ok = 200,
    bad_request = 400,
    forbidden = 403,
    not_found = 404,
    method_not_allowed = 405,
    internal_server_error = 500,
};

[[nodiscard]] std::vector<std::byte> make_response(
    Status status, std::span<const std::byte> body,
    std::string_view content_type, bool include_allow_get = false);
[[nodiscard]] std::vector<std::byte> make_error_response(Status status);
[[nodiscard]] std::string content_type_for_path(std::string_view path);

}  // namespace hp::http
