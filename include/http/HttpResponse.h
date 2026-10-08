#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/FileRegion.h"

namespace hp::http {

enum class ConnectionPolicy { kClose, kKeepAlive };

enum class Status {
  kOk = 200,
  kBadRequest = 400,
  kForbidden = 403,
  kNotFound = 404,
  kMethodNotAllowed = 405,
  kInternalServerError = 500,
};

struct ResponseResult {
  std::vector<std::byte> bytes;
  ConnectionPolicy effectivePolicy{ConnectionPolicy::kClose};
  std::optional<base::FileRegion> file{};
  Status status{Status::kInternalServerError};
  std::size_t contentBytes{0};
};

[[nodiscard]] std::vector<std::byte> makeResponseHeader(
    Status status,
    std::size_t contentLength,
    std::string_view contentType,
    bool includeAllowGet = false,
    ConnectionPolicy policy = ConnectionPolicy::kClose);

[[nodiscard]] std::vector<std::byte> makeResponse(
    Status status,
    std::span<const std::byte> body,
    std::string_view contentType,
    bool includeAllowGet = false,
    ConnectionPolicy policy = ConnectionPolicy::kClose);
[[nodiscard]] std::vector<std::byte> makeErrorResponse(
    Status status,
    ConnectionPolicy policy = ConnectionPolicy::kClose);

[[nodiscard]] std::size_t errorContentBytes(Status status);

[[nodiscard]] std::string contentTypeForPath(std::string_view path);

}  // namespace hp::http
