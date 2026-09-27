#pragma once
#include <chrono>
#include <optional>

#include "net/Channel.h"
#include "net/ConnectionIo.h"

namespace hp::net {
class TcpServer;
struct TcpConnectionTestAccess;

// 地址稳定，归属单一线程。关闭回调不得抛出异常，也不得
// 销毁连接；只有当前活动
// 回调返回后才允许销毁。
class TcpConnection final : private base::NonCopyable {
 public:
  // Consume 会使借用的输入失效；不得在回调返回后保留它。
  using MessageCallback = std::function<void(TcpConnection&, std::span<const std::byte>, bool)>;
  using Identity = std::uint64_t;
  using TimeoutActivityCallback = std::function<void(TcpConnection&, bool)>;
  using CloseCallback = std::function<void(int, Identity)>;
  // 一条 TCP 连接的状态
  enum class State { kUnregistered, kActive, kClosing };

  TcpConnection(EventLoop& ownerEventLoop,
                Socket socket,
                Identity identity,
                std::size_t maxInputBytes);
  void setMessageCallback(MessageCallback messageCallback) {
    messageCallback_ = std::move(messageCallback);
  }

  void setCloseCallback(CloseCallback closeCallback) { closeCallback_ = std::move(closeCallback); }

  void setTimeoutActivityCallback(TimeoutActivityCallback timeoutActivityCallback) {
    timeoutActivityCallback_ = std::move(timeoutActivityCallback);
  }

  ~TcpConnection() noexcept;

  void activateConnection();
  void requestClose() noexcept;

  void sendBytes(std::span<const std::byte> bytes);
  void sendFile(std::span<const std::byte> header, base::FileRegion file);
  void consumeInputBytes(std::size_t count);

  void closeAfterFlush();
  void beginConnectionDrain();

  void pauseReading();
  void resumeReading();

  void setIdleWait(bool waiting);

  using WriteCompleteCallback = std::function<void(TcpConnection&)>;

  void setWriteCompleteCallback(WriteCompleteCallback writeCompleteCallback) {
    writeCompleteCallback_ = std::move(writeCompleteCallback);
  }

  [[nodiscard]] std::span<const std::byte> inputView() const noexcept { return io_.inputView(); }

  [[nodiscard]] bool peerClosed() const noexcept { return io_.peerHalfClosed(); }

  [[nodiscard]] std::size_t pendingBytes() const noexcept { return io_.pendingBytes(); }

  // 所有者清理：注销，但不通知可能正在析构的所有者。
  void deactivateConnection() noexcept;

  [[nodiscard]] int fd() const noexcept { return io_.fd(); }
  [[nodiscard]] Identity identity() const noexcept { return identity_; }
  [[nodiscard]] State state() const noexcept { return state_; }

 private:
  friend struct GracefulShutdownTestAccess;
  friend struct SendfileTestAccess;
  friend struct ConnectionTimeoutTestAccess;
  friend class TcpServer;
  friend class ConnectionRegistry;
  friend struct TcpConnectionTestAccess;

  void dispatchMessage(std::span<const std::byte> input, bool eof);
  static void handleEchoMessage(TcpConnection& connection,
                                std::span<const std::byte> input,
                                bool eof);
  // 由 `Channel::eventCallback_` 保存
  void handleConnectionEvent(std::uint32_t mask) noexcept;
  void updateInterest();

  void readMessages();
  void flushOutput();

  TimeoutActivityCallback timeoutActivityCallback_;
  bool idleWaiting_{false};
  std::chrono::steady_clock::time_point lastProgress_{};
  std::optional<std::chrono::steady_clock::time_point> waitSince_;
  std::uint64_t timeoutId_{0};

  ConnectionIo io_;
  MessageCallback messageCallback_;  // 绑定 `HttpMessageHandler::onMessage()`
  const Identity identity_;
  CloseCallback closeCallback_;  // 绑定 `ConnectionRegistry::onClose()`
  Channel connectionChannel_;

  bool inputStopped_{false};
  bool draining_{false};
  bool readPaused_{false};
  WriteCompleteCallback writeCompleteCallback_;  // 绑定 `Session::onWriteComplete()`

  // 在 handleConnectionEvent() 期间被置 true
  bool handlingEvent_{false};
  bool eofNotified_{false};
  std::size_t messageCount_{0};
  State state_{State::kUnregistered};

  // 侵入式的所有者回收记录：事件请求关闭时
  // 无需分配内存。
  TcpConnection* nextClosing_{nullptr};
  bool queuedForRecovery_{false};

  std::size_t eventCount_{0};
  std::uint32_t lastMask_{0};
  ConnectionEventResult lastResult_;
};
}  // namespace hp::net
