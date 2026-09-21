#pragma once
#include <atomic>

#include "net/Acceptor.h"
#include "net/ConnectionRegistry.h"
#include "net/EventLoopThreadPool.h"

namespace hp::net {
struct TcpServerTestAccess;

class TcpServer final : private base::NonCopyable {
 public:
  using MessageFactoryCallback =
      std::function<TcpConnection::MessageCallback()>;

  explicit TcpServer(std::uint16_t requestedPort,
                     std::size_t maxInputBytes = 0,
                     std::size_t workerCount = 0,
                     ConnectionTimeouts timeouts = {});
  ~TcpServer() noexcept;
  void registerMessageFactoryCallback(
      MessageFactoryCallback messageFactoryCallback);

  [[nodiscard]] std::uint16_t boundPort() const noexcept;

  void run();

  // Thread-safe immediate stop, not signal-safe or graceful HTTP draining.
  void requestStop();
  void requestGracefulShutdown(EventLoop::Deadline deadline);
  void forceShutdown();

  // Owner-only attachment, initially disabled. Register the named signal target
  // and enable interest before Run. Server owns/removes the returned Channel.
  Channel& watchControlFd(int fd);

 private:
  friend struct GracefulShutdownTestAccess;
  friend struct SendfileTestAccess;
  friend struct ConnectionTimeoutTestAccess;
  friend struct TcpServerTestAccess;

  void setMessageFactoryCallback(
      MessageFactoryCallback messageFactoryCallback) {
    messageFactoryCallback_ = std::move(messageFactoryCallback);
  }

  struct ConnectionHandoff {
    Socket socket;
    TcpConnection::MessageCallback messageCallback;
  };

  void onAccepted(Socket socket);
  void initializeWorkerRegistry(std::size_t index, EventLoop& loop);
  void cleanupWorkerRegistry(std::size_t index, EventLoop& worker);
  void adoptConnection(std::size_t index,
                       const std::shared_ptr<ConnectionHandoff>& handoff,
                       EventLoop& loop);
  void shutdown();
  void onControl(std::size_t index,
                 EventLoop::Control kind,
                 EventLoop::Deadline deadline);
  void onControl(EventLoop::Control kind, EventLoop::Deadline deadline);

  EventLoop loop_;
  MessageFactoryCallback messageFactoryCallback_;
  std::size_t maxInputBytes_;
  const std::size_t workerCount_;
  const ConnectionTimeouts timeouts_;

  std::size_t nextWorker_{0};
  std::atomic<bool> stopping_{false}, workerFailed_{false};
  bool ran_{false}, draining_{false};
  std::atomic<std::size_t> workersFinished_{0};

  std::unique_ptr<ConnectionRegistry> mainRegistry_;
  std::vector<std::unique_ptr<ConnectionRegistry>> registries_;
  EventLoopThreadPool pool_;

  Acceptor acceptor_;
  std::unique_ptr<Channel> controlChannel_;
};
}  // namespace hp::net
