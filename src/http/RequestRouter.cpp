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
  const auto parsed = parseRequestTarget(request.target);
  if (!parsed.target) return rejectRequest(parsed.status, request.method == "HEAD", {});
  if (auto rejected = admitTarget(*parsed.target, request.method)) return std::move(*rejected);
  if (request.method == "POST") return echoResponse(request, policy);
  return service_.onValidatedTarget(*parsed.target, request.method == "HEAD", policy);
}

std::optional<ResponseResult> RequestRouter::onRequestHeaders(const RequestHead& head,
                                                              ConnectionPolicy) const {
  const auto parsed = parseRequestTarget(head.target);
  if (!parsed.target) return rejectRequest(parsed.status, head.method == "HEAD", {});
  return admitTarget(*parsed.target, head.method);
}

std::optional<ResponseResult> RequestRouter::admitTarget(const RequestTarget& target,
                                                         std::string_view method) const {
  const bool omitBody = method == "HEAD";
  if (target.path() == "/api/echo") {
    if (method == "POST") return std::nullopt;
    return rejectRequest(Status::kMethodNotAllowed, omitBody, "POST");
  }
  if (method == "GET" || omitBody) return std::nullopt;
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
