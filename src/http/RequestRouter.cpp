#include "http/RequestRouter.h"

namespace hp::http {
namespace {
ResponseResult rejectRequest(Status status, bool omitBody, std::string_view allowedMethods) {
  return {makeErrorResponse(status, ConnectionPolicy::kClose, omitBody, allowedMethods),
          ConnectionPolicy::kClose,
          std::nullopt,
          status,
          omitBody ? 0 : errorContentBytes(status)};
}
}  // namespace

ResponseResult RequestRouter::onResponse(const HttpRequest& request,
                                         ConnectionPolicy policy) const {
  const bool omitBody = request.method == "HEAD";
  if (request.target.find('%') != std::string::npos)
    return rejectRequest(Status::kBadRequest, omitBody, {});
  const std::string_view target(request.target);
  const auto path = target.substr(0, target.find('?'));
  if (path == "/api/echo") {
    if (request.method == "POST") return echoResponse(request, policy);
    return rejectRequest(Status::kMethodNotAllowed, omitBody, "POST");
  }
  if (request.method == "GET" || omitBody) return service_.onResponse(request, policy);
  return rejectRequest(Status::kMethodNotAllowed, false, "GET, HEAD");
}

ResponseResult RequestRouter::echoResponse(const HttpRequest& request,
                                           ConnectionPolicy policy) const {
  const auto body = std::as_bytes(std::span(request.body));
  return {makeResponse(Status::kOk, body, "application/octet-stream", std::string_view{}, policy),
          policy,
          std::nullopt,
          Status::kOk,
          body.size()};
}
}  // namespace hp::http
