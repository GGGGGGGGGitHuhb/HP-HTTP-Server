#pragma once
#include <memory>
#include <unordered_map>

#include "metrics/ServerMetrics.h"
#include "net/ConnectionTimeouts.h"
#include "net/EventLoop.h"
#include "net/TcpConnection.h"

namespace hp::net {
struct TcpServerTestAccess;

// 一个所属事件循环对应一个注册表。所有模式共用同一回收算法。
class ConnectionRegistry final : private base::NonCopyable {
 public:
  using StopCallback = EventLoop::TaskCallback;

  ConnectionRegistry(EventLoop& ownerEventLoop,
                     std::size_t maxInputBytes,
                     ConnectionTimeouts timeouts = {},
                     metrics::ServerMetrics* metrics = nullptr);
  ~ConnectionRegistry() noexcept;

  void setStopCallback(StopCallback stopCallback) { stopCallback_ = std::move(stopCallback); }

  void beginConnectionsDrain(bool force = false);

  void onAccepted(Socket socket, TcpConnection::MessageCallback messageCallback);
  // 把连接加入待清理链
  void onClose(int fd, TcpConnection::Identity identity) noexcept;
  // 从 ConnectionRegisry::connections_ 表中移除连接对象
  void onCleanup() noexcept;

 private:
  friend struct GracefulShutdownTestAccess;
  friend struct ConnectionTimeoutTestAccess;
  friend struct TcpServerTestAccess;

  void onTimeoutActivity(TcpConnection& connection, bool progress);
  void cancelTimeout(TcpConnection& connection) noexcept;
  void expireConnection(int fd, TcpConnection::Identity identity);

  bool draining_{false}, notified_{false};
  StopCallback stopCallback_;

  metrics::ServerMetrics* metrics_;
  const ConnectionTimeouts timeouts_;
  EventLoop& ownerEventLoop_;
  std::size_t maxInputBytes_;

  std::unordered_map<int, std::unique_ptr<TcpConnection>> connections_;
  TcpConnection* closingHead_{nullptr};
  TcpConnection::Identity nextIdentity_{1};
  TcpConnection::Identity registeringIdentity_{0};
};
}  // namespace hp::net
