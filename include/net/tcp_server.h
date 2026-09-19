#pragma once
#include <atomic>

#include "net/acceptor.h"
#include "net/connection_registry.h"
#include "net/event_loop_thread_pool.h"

namespace hp::net {
struct TcpServerTestAccess;

class TcpServer final : private base::NonCopyable {
 public:
  using MessageCallbackFactory =
      std::function<TcpConnection::MessageCallback()>;

  explicit TcpServer(std::uint16_t requested_port,
                     std::size_t max_input_bytes = 0,
                     std::size_t worker_count = 0,
                     ConnectionTimeouts timeouts = {});
  ~TcpServer() noexcept;
  void set_CreateMessageCallback_callback(
      MessageCallbackFactory message_callback_factory);

  [[nodiscard]] std::uint16_t bound_port() const noexcept;

  void Run();

  // Thread-safe immediate stop, not signal-safe or graceful HTTP draining.
  void RequestStop();
  void RequestGracefulShutdown(EventLoop::Deadline deadline);
  void ForceShutdown();

  // Owner-only attachment, initially disabled. Register the named signal target
  // and enable interest before Run. Server owns/removes the returned Channel.
  Channel& WatchControlFd(int fd);

 private:
  friend struct GracefulShutdownTestAccess;
  friend struct SendfileTestAccess;
  friend struct ConnectionTimeoutTestAccess;
  friend struct TcpServerTestAccess;

  struct ConnectionHandoff {
    Socket socket;
    TcpConnection::MessageCallback message_callback;
  };

  void AddConnection(Socket socket);
  void InitializeWorkerRegistry(std::size_t index, EventLoop& loop);
  void CleanupWorkerRegistry(std::size_t index, EventLoop& worker);
  void AdoptConnection(std::size_t index,
                       const std::shared_ptr<ConnectionHandoff>& handoff,
                       EventLoop& loop);
  void Shutdown();
  void HandleWorkerControl(std::size_t index,
                           EventLoop::Control kind,
                           EventLoop::Deadline deadline);
  void HandleControl(EventLoop::Control kind, EventLoop::Deadline deadline);

  EventLoop loop_;
  MessageCallbackFactory message_callback_factory_;
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
}  // namespace hp::net
