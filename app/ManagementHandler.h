#pragma once
#include "HttpConnectionHandler.h"
#include "base/AsyncLogger.h"

namespace hp::app {
enum class Lifecycle { kStarting, kReady, kStopping, kFailed };
class ManagementHandler {
 public:
  ManagementHandler(std::atomic<Lifecycle>& lifecycle,
                    const metrics::ServerMetrics& metrics,
                    const base::ResourceBudget& businessBudget,
                    const base::LoggerSession& logging);
  http::ResponseResult onResponse(const http::HttpRequest& request, http::ConnectionPolicy) const;
  std::optional<http::ResponseResult> onRequestHeaders(const http::RequestHead& head,
                                                       http::ConnectionPolicy) const;
  net::TcpConnection::MessageCallback onMessageFactory(
      std::shared_ptr<base::ResourceBudget> budget) const;

 private:
  std::string metricsText() const;
  std::atomic<Lifecycle>& lifecycle_;
  const metrics::ServerMetrics& metrics_;
  const base::ResourceBudget& businessBudget_;
  const base::LoggerSession& logging_;
};
}  // namespace hp::app
