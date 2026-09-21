#pragma once
#include <memory>
#include <unordered_map>

#include "net/ConnectionTimeouts.h"
#include "net/EventLoop.h"
#include "net/TcpConnection.h"

namespace hp::net {
struct TcpServerTestAccess;

// One owner loop, one registry. The same recovery algorithm serves all modes.
class ConnectionRegistry final : private base::NonCopyable {
 public:
  using StopCallback = EventLoop::TaskCallback;

  ConnectionRegistry(EventLoop& loop,
                     std::size_t maxInputBytes,
                     ConnectionTimeouts timeouts = {});
  ~ConnectionRegistry() noexcept;

  void setStopCallback(StopCallback stopCallback) {
    stopCallback_ = std::move(stopCallback);
  }

  void beginDrain(bool force = false);

  void onAccepted(Socket socket,
                  TcpConnection::MessageCallback messageCallback);
  void onClose(int fd, TcpConnection::Identity identity) noexcept;
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

  const ConnectionTimeouts timeouts_;
  EventLoop& loop_;
  std::size_t maxInputBytes_;

  std::unordered_map<int, std::unique_ptr<TcpConnection>> connections_;
  TcpConnection* closingHead_{nullptr};
  TcpConnection::Identity nextIdentity_{1};
};
}  // namespace hp::net
