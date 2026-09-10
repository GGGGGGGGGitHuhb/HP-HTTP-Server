#pragma once
#include <atomic>
#include "net/acceptor.h"
#include "net/connection_registry.h"
#include "net/event_loop_thread_pool.h"

namespace hp::net {
struct TcpServerTestAccess;
class TcpServer final : private base::NonCopyable {
public:
    using MessageCallbackFactory = std::function<TcpConnection::MessageCallback()>;
    explicit TcpServer(std::uint16_t requested_port, MessageCallbackFactory factory = {},
                       std::size_t max_input_bytes = 0, std::size_t worker_count = 0,
                       ConnectionTimeouts timeouts = {});
    ~TcpServer() noexcept;
    [[nodiscard]] std::uint16_t bound_port() const noexcept;
    void run();
    // Thread-safe immediate stop, not signal-safe or graceful HTTP draining.
    void request_stop();
    void request_graceful_shutdown(EventLoop::Deadline deadline);
    void force_shutdown();
    // Owner-only attachment; server removes this Channel before destroying its loop.
    void watch_control_fd(int fd, Channel::Callback callback);
private:
    friend struct GracefulShutdownTestAccess;
    friend struct SendfileTestAccess;
    friend struct ConnectionTimeoutTestAccess;
    friend struct TcpServerTestAccess;
    void add_connection(Socket socket);
    void shutdown();
    void control(EventLoop::Control kind, EventLoop::Deadline deadline);
    EventLoop loop_;
    MessageCallbackFactory callback_factory_;
    std::size_t max_input_bytes_;
    const std::size_t worker_count_;
    const ConnectionTimeouts timeouts_;
    std::size_t next_worker_{0};
    std::atomic<bool> stopping_{false}, worker_failed_{false};
    bool ran_{false}, draining_{false};
    std::atomic<std::size_t> workers_finished_{0};
    std::unique_ptr<ConnectionRegistry> main_registry_;
    std::vector<std::unique_ptr<ConnectionRegistry>> registries_;
    EventLoopThreadPool pool_;
    Acceptor acceptor_;
    std::unique_ptr<Channel> control_channel_;
};
}
