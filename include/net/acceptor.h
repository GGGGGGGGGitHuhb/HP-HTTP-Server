#pragma once
#include "net/channel.h"
#include "net/socket.h"

namespace hp::net {
struct AcceptorTestAccess;

// Single loop thread. The receiver takes sole ownership of each accepted Socket.
class Acceptor final : private base::NonCopyable {
public:
    using AcceptedCallback = std::function<void(Socket)>;
    Acceptor(EventLoop& loop, std::uint16_t port, AcceptedCallback callback);
    ~Acceptor() noexcept;
    void start();
    void stop() noexcept;
    void close() noexcept;

    [[nodiscard]] std::uint16_t bound_port() const noexcept {
        return bound_port_;
    }

private:
    friend struct AcceptorTestAccess;
    static Socket create_listener(std::uint16_t port);
    void handle_event(std::uint32_t mask);
    void accept_ready();
    Socket listener_;
    AcceptedCallback callback_;
    const std::uint16_t bound_port_;
    Channel channel_;
};
}
