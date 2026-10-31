#include "ManagementHandler.h"

namespace hp::app {
namespace {
http::ResponseResult managementResponse(http::Status status, std::string body, bool head) {
  if (body.size() > 32768) {
    status = http::Status::kInternalServerError;
    body = "snapshot too large\n";
  }
  auto bytes = http::makeResponseHeader(status,
                                        body.size(),
                                        "text/plain",
                                        status == http::Status::kMethodNotAllowed ? "GET" : "",
                                        http::ConnectionPolicy::kClose);
  const std::string_view cache = "Cache-Control: no-store\r\n";
  bytes.insert(bytes.end() - 2,
               reinterpret_cast<const std::byte*>(cache.data()),
               reinterpret_cast<const std::byte*>(cache.data() + cache.size()));
  if (!head)
    bytes.insert(bytes.end(),
                 reinterpret_cast<const std::byte*>(body.data()),
                 reinterpret_cast<const std::byte*>(body.data() + body.size()));
  return {std::move(bytes), http::ConnectionPolicy::kClose, {}, status, head ? 0 : body.size()};
}
}  // namespace

ManagementHandler::ManagementHandler(std::atomic<Lifecycle>& lifecycle,
                                     const metrics::ServerMetrics& metrics,
                                     const base::ResourceBudget& businessBudget,
                                     const base::LoggerSession& logging)
    : lifecycle_(lifecycle),
      metrics_(metrics),
      businessBudget_(businessBudget),
      logging_(logging) {}

std::string ManagementHandler::metricsText() const {
  const auto stats = logging_.stats();
  std::string result = metrics::ServerMetrics::serializeSnapshot(metrics_.snapshot()) +
                       base::ResourceBudget::serializeSnapshot(businessBudget_.snapshot());
  result += "logger_submitted " + std::to_string(stats.submitted) + "\nlogger_accepted " +
            std::to_string(stats.accepted) + "\nlogger_dropped_full " +
            std::to_string(stats.droppedFull) + "\nlogger_rejected_stopped " +
            std::to_string(stats.rejectedStopped) + "\nlogger_truncated " +
            std::to_string(stats.truncated) + "\nlogger_written " + std::to_string(stats.written) +
            "\nlogger_failed " + std::to_string(stats.failed) + "\nlogger_pending " +
            std::to_string(stats.pending) + "\n";
  return result;
}

http::ResponseResult ManagementHandler::onResponse(const http::HttpRequest& request,
                                                   http::ConnectionPolicy) const {
  const bool head = request.method == "HEAD";
  if (request.target != "/healthz" && request.target != "/readyz" && request.target != "/metrics")
    return managementResponse(http::Status::kNotFound, "not found\n", head);
  if (request.method != "GET")
    return managementResponse(http::Status::kMethodNotAllowed, "method not allowed\n", head);
  if (!request.body.empty())
    return managementResponse(http::Status::kBadRequest, "body not allowed\n", false);
  if (request.target == "/healthz") return managementResponse(http::Status::kOk, "ok\n", false);
  if (request.target == "/metrics")
    return managementResponse(http::Status::kOk, metricsText(), false);
  const bool ready = lifecycle_.load() == Lifecycle::kReady;
  return managementResponse(ready ? http::Status::kOk : http::Status::kServiceUnavailable,
                            ready ? "ready\n" : "not ready\n",
                            false);
}

std::optional<http::ResponseResult> ManagementHandler::onRequestHeaders(
    const http::RequestHead& head,
    http::ConnectionPolicy) const {
  return managementResponse(http::Status::kExpectationFailed,
                            "expectation failed\n",
                            head.method == "HEAD");
}

net::TcpConnection::MessageCallback ManagementHandler::onMessageFactory(
    std::shared_ptr<base::ResourceBudget> budget) const {
  HttpMessageHandler handler;
  handler.resourceBudget = std::move(budget);
  handler.requestReadTimeout = std::chrono::milliseconds{2000};
  handler.setRequestHeadersCallback(
      [this](const auto& head, auto policy) { return onRequestHeaders(head, policy); });
  handler.setResponseCallback(
      [this](const auto& request, auto policy) { return onResponse(request, policy); });
  return [handler = std::move(handler)](auto& connection, auto bytes, bool eof) mutable {
    handler.onMessage(connection, bytes, eof);
  };
}
}  // namespace hp::app
