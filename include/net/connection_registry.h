#pragma once
#include "net/event_loop.h"
#include "net/tcp_connection.h"
#include <memory>
#include <unordered_map>

namespace hp::net {
struct TcpServerTestAccess;
// One owner loop, one registry. The same recovery algorithm serves all modes.
class ConnectionRegistry final : private base::NonCopyable {
public:
    ConnectionRegistry(EventLoop& loop, std::size_t max_input_bytes);
    ~ConnectionRegistry() noexcept;
    void add(Socket socket, TcpConnection::MessageCallback callback);
    void connection_closed(int fd, TcpConnection::Identity identity) noexcept;
    void drain_closed_connections() noexcept;
private:
    friend struct TcpServerTestAccess;
    EventLoop& loop_;
    std::size_t max_input_bytes_;
    std::unordered_map<int, std::unique_ptr<TcpConnection>> connections_;
    TcpConnection* closing_head_{nullptr};
    TcpConnection::Identity next_identity_{1};
};
}
