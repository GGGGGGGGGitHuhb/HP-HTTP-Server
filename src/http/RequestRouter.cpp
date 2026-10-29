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
  if (auto rejected =
          onRequestHeaders({request.method, request.target, request.closeRequested}, policy))
    return std::move(*rejected);
  if (request.method == "POST") return echoResponse(request, policy);
  return service_.onResponse(request, policy);
}

std::optional<ResponseResult> RequestRouter::onRequestHeaders(const RequestHead& head,
                                                              ConnectionPolicy) const {
  const bool omitBody = head.method == "HEAD";
  if (head.target.find('%') != std::string::npos)
    return rejectRequest(Status::kBadRequest, omitBody, {});
  const std::string_view target(head.target);
  const auto path = target.substr(0, target.find('?'));
  if (path == "/api/echo") {
    if (head.method == "POST") return std::nullopt;
    return rejectRequest(Status::kMethodNotAllowed, omitBody, "POST");
  }
  if (head.method == "GET" || omitBody) return std::nullopt;
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
