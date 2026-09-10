#pragma once
#include "net/event_loop.h"
#include "net/connection_timeouts.h"
#include "net/tcp_connection.h"
#include <memory>
#include <unordered_map>

namespace hp::net {
struct TcpServerTestAccess;

// One owner loop, one registry. The same recovery algorithm serves all modes.
class ConnectionRegistry final : private base::NonCopyable {
public:
    ConnectionRegistry(EventLoop& loop, std::size_t max_input_bytes,
                       ConnectionTimeouts timeouts = {});
    ~ConnectionRegistry() noexcept;
    void set_drained_callback(EventLoop::Task callback);
    void begin_drain(bool force = false);
    void add(Socket socket, TcpConnection::MessageCallback callback);
    void connection_closed(int fd, TcpConnection::Identity identity) noexcept;
    void drain_closed_connections() noexcept;

private:
    friend struct GracefulShutdownTestAccess;
    friend struct ConnectionTimeoutTestAccess;
    friend struct TcpServerTestAccess;
    void update_timeout(TcpConnection& connection, bool progress);
    void cancel_timeout(TcpConnection& connection) noexcept;
    void expire(int fd, TcpConnection::Identity identity);
    bool draining_{false}, notified_{false};
    EventLoop::Task drained_callback_;
    const ConnectionTimeouts timeouts_;
    EventLoop& loop_;
    std::size_t max_input_bytes_;
    std::unordered_map<int, std::unique_ptr<TcpConnection>> connections_;
    TcpConnection* closing_head_{nullptr};
    TcpConnection::Identity next_identity_{1};
};
}
