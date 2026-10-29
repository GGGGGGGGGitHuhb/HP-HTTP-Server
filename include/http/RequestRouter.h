#pragma once

#include "http/StaticFileService.h"

namespace hp::http {
class RequestRouter final {
 public:
  explicit RequestRouter(const StaticFileService& service) : service_(service) {}

  [[nodiscard]] ResponseResult onResponse(const HttpRequest& request,
                                          ConnectionPolicy policy) const;

  [[nodiscard]] std::optional<ResponseResult> onRequestHeaders(const RequestHead& head,
                                                               ConnectionPolicy policy) const;

 private:
  [[nodiscard]] std::optional<ResponseResult> admitTarget(const RequestTarget& target,
                                                          std::string_view method) const;

  [[nodiscard]] ResponseResult echoResponse(const HttpRequest& request,
                                            ConnectionPolicy policy) const;

  const StaticFileService& service_;
};
}  // namespace hp::http
