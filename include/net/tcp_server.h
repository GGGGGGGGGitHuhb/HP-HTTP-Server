#pragma once
#include <memory>
#include <unordered_map>
#include "net/acceptor.h"
#include "net/event_loop.h"
#include "net/tcp_connection.h"

namespace hp::net {
struct TcpServerTestAccess;
class TcpServer final : private base::NonCopyable {
public:
    using MessageCallbackFactory = std::function<TcpConnection::MessageCallback()>;
    explicit TcpServer(std::uint16_t requested_port, MessageCallbackFactory factory = {},
                       std::size_t max_input_bytes = 0);
    ~TcpServer() noexcept;
    [[nodiscard]] std::uint16_t bound_port() const noexcept;
    [[noreturn]] void run();
private:
    friend struct TcpServerTestAccess;
    void add_connection(Socket socket);
    void connection_closed(int fd, TcpConnection::Identity identity) noexcept;
    void drain_closed_connections() noexcept;
    EventLoop loop_;
    std::unordered_map<int, std::unique_ptr<TcpConnection>> connections_;
    TcpConnection* closing_head_{nullptr};
    MessageCallbackFactory callback_factory_;
    std::size_t max_input_bytes_{0};
    TcpConnection::Identity next_identity_{1};
    Acceptor acceptor_;
};
}
