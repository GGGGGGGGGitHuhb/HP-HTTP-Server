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
    // Borrowed input is invalidated by consume; never retain it after callback.
    using MessageCallback = std::function<void(TcpConnection&, std::span<const std::byte>, bool)>;
    using Identity = std::uint64_t;
    using CloseCallback = std::function<void(int, Identity)>;
    enum class State { unregistered, active, closing };
    TcpConnection(EventLoop& loop, Socket socket, Identity identity,
                  MessageCallback handler, std::size_t max_input_bytes,
                  CloseCallback close_callback);
    ~TcpConnection() noexcept;
    void start();
    void request_close() noexcept;
    void send(std::span<const std::byte> bytes);
    void consume(std::size_t count);
    void close_after_flush();
    void pause_reading();
    void resume_reading();
    using WriteCompleteCallback = std::function<void(TcpConnection&)>;
    void set_write_complete_callback(WriteCompleteCallback callback);
    [[nodiscard]] std::span<const std::byte> input_view() const noexcept { return io_.input_view(); }
    [[nodiscard]] bool peer_closed() const noexcept { return io_.peer_half_closed(); }
    [[nodiscard]] std::size_t pending_bytes() const noexcept { return io_.pending_bytes(); }
    // Owner teardown: unregister without notifying a possibly destructing owner.
    void stop() noexcept;
    [[nodiscard]] int fd() const noexcept { return io_.fd(); }
    [[nodiscard]] Identity identity() const noexcept { return identity_; }
    [[nodiscard]] State state() const noexcept { return state_; }
private:
    friend class TcpServer;
    friend class ConnectionRegistry;
    friend struct TcpConnectionTestAccess;
    void handle_event(std::uint32_t mask) noexcept;
    void update_interest();
    void read_messages();
    void flush_output();
    ConnectionIo io_;
    MessageCallback message_callback_;
    const Identity identity_;
    CloseCallback close_callback_;
    Channel channel_;
    bool input_stopped_{false};
    bool read_paused_{false};
    WriteCompleteCallback write_complete_callback_;
    bool handling_event_{false};
    bool eof_notified_{false};
    std::size_t message_count_{0};
    State state_{State::unregistered};
    // Intrusive owner recovery record: no allocation when an event requests close.
    TcpConnection* next_closing_{nullptr};
    bool queued_for_recovery_{false};
    std::size_t event_count_{0};
    std::uint32_t last_mask_{0};
    ConnectionEventResult last_result_;
};
}
