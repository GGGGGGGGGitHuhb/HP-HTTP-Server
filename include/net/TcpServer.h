#pragma once
#include <atomic>

#include "net/Acceptor.h"
#include "net/ConnectionRegistry.h"
#include "net/EventLoopThreadPool.h"

namespace hp::net {
struct TcpServerTestAccess;

class TcpServer final : private base::NonCopyable {
 public:
  using MessageFactoryCallback = std::function<TcpConnection::MessageCallback()>;

  explicit TcpServer(std::uint16_t requestedPort,
                     std::size_t maxInputBytes = 0,
                     std::size_t workerCount = 0,
                     ConnectionTimeouts timeouts = {},
                     metrics::ServerMetrics* metrics = nullptr);
  ~TcpServer() noexcept;
  void registerMessageFactoryCallback(MessageFactoryCallback messageFactoryCallback);

  [[nodiscard]] std::uint16_t boundPort() const noexcept;

  void runTcpServer();

  // 线程安全的立即停止；不保证信号安全，也不执行 HTTP 优雅排空。
  void requestServerStop();
  void requestServerGracefulShutdown(EventLoop::Deadline deadline);
  void requestServerForceClose();

  // 仅所属线程可挂接，初始为禁用。注册具名信号处理目标，
  // 并在 Run 前启用关注事件。服务器拥有并移除返回的 Channel。
  Channel& watchControlFd(int fd);

 private:
  friend struct GracefulShutdownTestAccess;
  friend struct SendfileTestAccess;
  friend struct ConnectionTimeoutTestAccess;
  friend struct TcpServerTestAccess;

  void setMessageFactoryCallback(MessageFactoryCallback messageFactoryCallback) {
    messageFactoryCallback_ = std::move(messageFactoryCallback);
  }

  struct ConnectionHandoff {
    Socket socket;
    TcpConnection::MessageCallback messageCallback;
  };

  void onAccepted(Socket socket);
  void initializeWorkerRegistry(std::size_t workerIndex, EventLoop& workerEventLoop);
  void cleanupWorkerRegistry(std::size_t workerIndex, EventLoop& workerEventLoop);
  void adoptConnection(std::size_t workerIndex,
                       const std::shared_ptr<ConnectionHandoff>& handoff,
                       EventLoop& workerEventLoop);
  void shutdownTcpServer();
  void onControl(std::size_t workerIndex, EventLoop::Control kind, EventLoop::Deadline deadline);
  void onControl(EventLoop::Control kind, EventLoop::Deadline deadline);

  EventLoop mainEventLoop_;
  MessageFactoryCallback messageFactoryCallback_;  // 绑定 `onMessageFactory()`
  metrics::ServerMetrics* metrics_;
  std::size_t maxInputBytes_;
  const std::size_t workerCount_;
  const ConnectionTimeouts timeouts_;

  std::size_t nextWorkerIndex_{0};
  std::atomic<bool> stopping_{false}, workerFailed_{false};
  bool ran_{false}, draining_{false};
  std::atomic<std::size_t> workersFinished_{0};

  std::unique_ptr<ConnectionRegistry> mainConnectionRegistry_;  // 连接注册表
  std::vector<std::unique_ptr<ConnectionRegistry>> workerRegistries_;
  EventLoopThreadPool workerPool_;

  Acceptor acceptor_;
  std::unique_ptr<Channel> controlChannel_;
};
}  // namespace hp::net
