#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "base/NonCopyable.h"
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"

namespace hp::http {

inline constexpr std::size_t kMaxFileBytes = 8U * 1024U * 1024U;

class StaticFileService final : private base::NonCopyable {
 public:
  explicit StaticFileService(const std::string& rootPath);
  ~StaticFileService();

  [[nodiscard]] std::vector<std::byte> handle(
      const HttpRequest& request,
      ConnectionPolicy policy = ConnectionPolicy::kClose) const;

  [[nodiscard]] ResponseResult handleResponse(
      const HttpRequest& request,
      ConnectionPolicy policy = ConnectionPolicy::kClose) const;

  [[nodiscard]] ResponseResult onResponse(
      const HttpRequest& request,
      ConnectionPolicy policy = ConnectionPolicy::kClose) const;

 private:
  friend struct StaticFileServiceTestAccess;

  int rootFd_{-1};
};

}  // namespace hp::http
