#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include "base/file_region.h"

namespace hp::http {

enum class ConnectionPolicy { close, keep_alive };

struct ResponseResult {
    std::vector<std::byte> bytes;
    ConnectionPolicy effective_policy{ConnectionPolicy::close};
    std::optional<base::FileRegion> file{};
};

enum class Status {
    ok = 200,
    bad_request = 400,
    forbidden = 403,
    not_found = 404,
    method_not_allowed = 405,
    internal_server_error = 500,
};

[[nodiscard]] std::vector<std::byte> make_response_header(
    Status status, std::size_t content_length, std::string_view content_type,
    bool include_allow_get = false, ConnectionPolicy policy = ConnectionPolicy::close);

[[nodiscard]] std::vector<std::byte> make_response(
    Status status, std::span<const std::byte> body,
    std::string_view content_type, bool include_allow_get = false,
    ConnectionPolicy policy = ConnectionPolicy::close);
[[nodiscard]] std::vector<std::byte> make_error_response(Status status,
    ConnectionPolicy policy = ConnectionPolicy::close);
[[nodiscard]] std::string content_type_for_path(std::string_view path);

}  // namespace hp::http
