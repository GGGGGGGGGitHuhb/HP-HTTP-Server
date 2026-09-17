#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/file_region.h"

namespace hp::http {

enum class ConnectionPolicy { kClose, kKeepAlive };

struct ResponseResult {
  std::vector<std::byte> bytes;
  ConnectionPolicy effective_policy{ConnectionPolicy::kClose};
  std::optional<base::FileRegion> file{};
};

enum class Status {
  kOk = 200,
  kBadRequest = 400,
  kForbidden = 403,
  kNotFound = 404,
  kMethodNotAllowed = 405,
  kInternalServerError = 500,
};

[[nodiscard]] std::vector<std::byte> MakeResponseHeader(
    Status status,
    std::size_t content_length,
    std::string_view content_type,
    bool include_allow_get = false,
    ConnectionPolicy policy = ConnectionPolicy::kClose);

[[nodiscard]] std::vector<std::byte> MakeResponse(
    Status status,
    std::span<const std::byte> body,
    std::string_view content_type,
    bool include_allow_get = false,
    ConnectionPolicy policy = ConnectionPolicy::kClose);
[[nodiscard]] std::vector<std::byte> MakeErrorResponse(
    Status status,
    ConnectionPolicy policy = ConnectionPolicy::kClose);

[[nodiscard]] std::string ContentTypeForPath(std::string_view path);

}  // namespace hp::http
