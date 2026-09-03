#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "base/non_copyable.h"
#include "http/http_request.h"

namespace hp::http {

inline constexpr std::size_t max_file_bytes = 8U * 1024U * 1024U;

class StaticFileService final : private base::NonCopyable {
   public:
    explicit StaticFileService(const std::string& root_path);
    ~StaticFileService();

    [[nodiscard]] std::vector<std::byte> handle(
        const HttpRequest& request) const;

   private:
    int root_fd_{-1};
};

}  // namespace hp::http
