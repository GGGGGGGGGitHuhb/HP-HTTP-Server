#pragma once
#include "net/channel.h"
#include "net/connection_io.h"

namespace hp::net {
class TcpServer;
struct TcpConnectionTestAccess;
// Address-stable, single-threaded owner. Close callbacks must not throw or destroy
// the connection; destruction is permitted only after the active callback returns.
class TcpConnection final : private base::NonCopyable {
public:
    using Identity = std::uint64_t;
    using CloseCallback = std::function<void(int, Identity)>;
    enum class State { unregistered, active, closing };
    TcpConnection(EventLoop& loop, Socket socket, Identity identity,
                  ApplicationHandler handler, std::size_t max_input_bytes,
                  CloseCallback close_callback);
    ~TcpConnection() noexcept;
    void start();
    void request_close() noexcept;
    // Owner teardown: unregister without notifying a possibly destructing owner.
    void stop() noexcept;
    [[nodiscard]] int fd() const noexcept { return io_.fd(); }
    [[nodiscard]] Identity identity() const noexcept { return identity_; }
    [[nodiscard]] State state() const noexcept { return state_; }
private:
    friend class TcpServer;
    friend struct TcpConnectionTestAccess;
    void handle_event(std::uint32_t mask) noexcept;
    void update_interest();
    ConnectionIo io_;
    const Identity identity_;
    CloseCallback close_callback_;
    Channel channel_;
    State state_{State::unregistered};
    // Intrusive owner recovery record: no allocation when an event requests close.
    TcpConnection* next_closing_{nullptr};
    bool queued_for_recovery_{false};
    std::size_t event_count_{0};
    std::uint32_t last_mask_{0};
    ConnectionEventResult last_result_;
};
}
