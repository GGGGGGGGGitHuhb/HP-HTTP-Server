#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "base/non_copyable.h"
#include "http/http_request.h"
#include "http/http_response.h"

namespace hp::http {

inline constexpr std::size_t kMaxFileBytes = 8U * 1024U * 1024U;

class StaticFileService final : private base::NonCopyable {
 public:
  explicit StaticFileService(const std::string& root_path);
  ~StaticFileService();

  [[nodiscard]] std::vector<std::byte> Handle(
      const HttpRequest& request,
      ConnectionPolicy policy = ConnectionPolicy::kClose) const;

  [[nodiscard]] ResponseResult HandleResponse(
      const HttpRequest& request,
      ConnectionPolicy policy = ConnectionPolicy::kClose) const;

  [[nodiscard]] ResponseResult PrepareResponse(
      const HttpRequest& request,
      ConnectionPolicy policy = ConnectionPolicy::kClose) const;

 private:
  friend struct StaticFileServiceTestAccess;

  int root_fd_{-1};
};

}  // namespace hp::http
