#pragma once
#include <atomic>
#include <chrono>

#include "AccessLog.h"
#include "AsyncTaskHandler.h"
#include "http/RequestRouter.h"
#include "net/TcpServer.h"

namespace hp::app {
struct HttpCallbackStats {
  std::size_t callbacks{}, parses{}, needMore{}, responses{}, eofNotifications{};
  std::size_t submittedBytes{}, acceptedBytes{}, consumedBytes{};
};

using ResponseCallback =
    std::function<http::ResponseResult(const http::HttpRequest&, http::ConnectionPolicy)>;

using RequestHeadersCallback =
    std::function<std::optional<http::ResponseResult>(const http::RequestHead&,
                                                      http::ConnectionPolicy)>;

// 测试观察钩子。在创建会话前安装；在所属执行线程停止后
// 清除。
using SessionEventCallback = void (*)(bool, const void*) noexcept;
inline std::atomic<SessionEventCallback> sessionEventCallback{nullptr};
inline void setSessionEventCallback(SessionEventCallback newSessionEventCallback) {
  sessionEventCallback.store(newSessionEventCallback);
}

// 显式注册响应策略；Session 保持延迟创建，并绑定
// 原来的 IO 所属线程。首次分发前创建的副本保持独立
// 状态。
struct Session : std::enable_shared_from_this<Session> {
  enum class Phase { kReading, kInterimWriting, kAwaitingTask, kWriting, kClosing };
  RequestHeadersCallback requestHeadersCallback;
  ResponseCallback responseCallback;  // 绑定 `StaticFileService::onResponse()`
  HttpCallbackStats* stats;
  http::RequestParser parser;    // 解析器
  Phase phase{Phase::kReading};  // 当前 HTTP 会话的状态
  bool completed{false};
  // 当前响应结束后是否要求关闭
  bool close{false};

  metrics::ServerMetrics* metrics{};
  bool accessLogEnabled{};
  bool requestPending{};
  AccessRecord accessRecord;
  std::chrono::steady_clock::time_point requestStarted;
  std::chrono::milliseconds requestReadTimeout{0};
  std::optional<std::chrono::steady_clock::time_point> readDeadline;

  std::shared_ptr<const AsyncTaskHandler> asyncHandler;
  std::shared_ptr<business::TaskJob> pendingJob;
  std::uint64_t taskGeneration{};
  std::shared_ptr<base::ResourceBudget> resourceBudget;
  base::ResourceLease requestLease;
  void closeResourceRejected(net::TcpConnection& connection);
  void armTaskContinuation(net::TcpConnection& connection);
  void onTaskContinuation(net::TcpConnection& connection, std::uint64_t generation);
  void sendFinalResponse(net::TcpConnection& connection, http::ResponseResult result);

  explicit Session(HttpCallbackStats* callbackStats,
                   metrics::ServerMetrics* serverMetrics = nullptr,
                   bool enableAccessLog = false,
                   std::chrono::milliseconds readTimeout = std::chrono::milliseconds{0},
                   std::shared_ptr<base::ResourceBudget> resourceBudget = {});
  ~Session();
  void setRequestHeadersCallback(RequestHeadersCallback requestHeadersCallback) {
    this->requestHeadersCallback = std::move(requestHeadersCallback);
  }

  void setResponseCallback(ResponseCallback responseCallback) {
    this->responseCallback = std::move(responseCallback);
  }

  void onMessage(net::TcpConnection& connection);
  void onWriteComplete(net::TcpConnection& connection);
  void onRequestHeaders(net::TcpConnection& connection, const http::RequestHead& head);
  void finishPendingRequest(bool aborted) noexcept;
  bool closeExpiredRequest(net::TcpConnection& connection);
  void clearReadDeadline(net::TcpConnection& connection);
};

struct HttpMessageHandler {
  RequestHeadersCallback requestHeadersCallback;
  ResponseCallback responseCallback;  // 绑定 `StaticFileService::onResponse()`
  HttpCallbackStats* stats{};
  metrics::ServerMetrics* metrics{};
  bool accessLogEnabled{};
  std::shared_ptr<Session> session;
  std::chrono::milliseconds requestReadTimeout{0};

  std::shared_ptr<const AsyncTaskHandler> asyncHandler{};
  std::shared_ptr<base::ResourceBudget> resourceBudget{};

  void setRequestHeadersCallback(RequestHeadersCallback requestHeadersCallback) {
    this->requestHeadersCallback = std::move(requestHeadersCallback);
  }

  void setResponseCallback(ResponseCallback responseCallback) {
    this->responseCallback = std::move(responseCallback);
  }

  // 由 `TcpConnection::messageCallback_` 保存
  void onMessage(net::TcpConnection& connection, std::span<const std::byte>, bool);
};

struct HttpMessageFactory {
  const http::RequestRouter& router;
  metrics::ServerMetrics* metrics{};
  bool accessLogEnabled{};
  std::chrono::milliseconds requestReadTimeout{0};
  std::shared_ptr<const http::RequestRouter> ownedRouter{};
  std::shared_ptr<const AsyncTaskHandler> asyncHandler{};
  std::shared_ptr<base::ResourceBudget> resourceBudget{};
  // “为新连接创建 HTTP 消息回调”的工厂
  net::TcpConnection::MessageCallback onMessageFactory() const;
};

// 每个回调在连接所属线程上延迟创建自己的 Session。共享的
// 自定义提供方和统计数据需要调用方同步；service 的生命周期须覆盖所有
// 工作线程。
net::TcpConnection::MessageCallback makeHttpCallback(
    ResponseCallback responseCallback,
    HttpCallbackStats* stats = nullptr,
    metrics::ServerMetrics* metrics = nullptr,
    bool accessLogEnabled = false,
    std::chrono::milliseconds requestReadTimeout = std::chrono::milliseconds{0},
    std::shared_ptr<base::ResourceBudget> resourceBudget = {});
net::TcpServer::MessageFactoryCallback makeHttpFactory(
    const http::StaticFileService& service,
    std::chrono::milliseconds requestReadTimeout = std::chrono::milliseconds{0},
    std::shared_ptr<base::ResourceBudget> resourceBudget = {});
}  // namespace hp::app
