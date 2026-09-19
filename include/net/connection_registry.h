#pragma once
#include <memory>
#include <unordered_map>

#include "net/connection_timeouts.h"
#include "net/event_loop.h"
#include "net/tcp_connection.h"

namespace hp::net {
struct TcpServerTestAccess;

// One owner loop, one registry. The same recovery algorithm serves all modes.
class ConnectionRegistry final : private base::NonCopyable {
 public:
  ConnectionRegistry(EventLoop& loop,
                     std::size_t max_input_bytes,
                     ConnectionTimeouts timeouts = {});
  ~ConnectionRegistry() noexcept;

  void set_RequestStop_callback(EventLoop::LoopTask request_stop_callback);
  void BeginDrain(bool force = false);

  void AddConnection(Socket socket,
                     TcpConnection::MessageCallback message_callback);
  void OnConnectionClosed(int fd, TcpConnection::Identity identity) noexcept;
  void DrainClosedConnections() noexcept;

 private:
  friend struct GracefulShutdownTestAccess;
  friend struct ConnectionTimeoutTestAccess;
  friend struct TcpServerTestAccess;

  void UpdateTimeout(TcpConnection& connection, bool progress);
  void CancelTimeout(TcpConnection& connection) noexcept;
  void ExpireConnection(int fd, TcpConnection::Identity identity);

  bool draining_{false}, notified_{false};
  EventLoop::LoopTask request_stop_callback_;

  const ConnectionTimeouts timeouts_;
  EventLoop& loop_;
  std::size_t max_input_bytes_;

  std::unordered_map<int, std::unique_ptr<TcpConnection>> connections_;
  TcpConnection* closing_head_{nullptr};
  TcpConnection::Identity next_identity_{1};
};
}  // namespace hp::net
