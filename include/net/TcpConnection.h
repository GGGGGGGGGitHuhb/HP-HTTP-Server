#pragma once
#include <chrono>
#include <optional>

#include "net/Channel.h"
#include "net/ConnectionIo.h"

namespace hp::net {
class TcpServer;
struct TcpConnectionTestAccess;

// Address-stable, single-threaded owner. Close callbacks must not throw or
// destroy the connection; destruction is permitted only after the active
// callback returns.
class TcpConnection final : private base::NonCopyable {
 public:
  // Borrowed input is invalidated by Consume; never retain it after callback.
  using MessageCallback =
      std::function<void(TcpConnection&, std::span<const std::byte>, bool)>;
  using Identity = std::uint64_t;
  using TimeoutActivityCallback = std::function<void(TcpConnection&, bool)>;
  using CloseCallback = std::function<void(int, Identity)>;
  enum class State { kUnregistered, kActive, kClosing };

  TcpConnection(EventLoop& loop,
                Socket socket,
                Identity identity,
                std::size_t maxInputBytes);
  void setMessageCallback(MessageCallback messageCallback) {
    messageCallback_ = std::move(messageCallback);
  }

  void setCloseCallback(CloseCallback closeCallback) {
    closeCallback_ = std::move(closeCallback);
  }

  void setTimeoutActivityCallback(
      TimeoutActivityCallback timeoutActivityCallback) {
    timeoutActivityCallback_ = std::move(timeoutActivityCallback);
  }

  ~TcpConnection() noexcept;

  void start();
  void requestClose() noexcept;

  void send(std::span<const std::byte> bytes);
  void sendFile(std::span<const std::byte> header, base::FileRegion file);
  void consume(std::size_t count);

  void closeAfterFlush();
  void beginDrain();

  void pauseReading();
  void resumeReading();

  void setIdleWait(bool waiting);

  using WriteCompleteCallback = std::function<void(TcpConnection&)>;

  void setWriteCompleteCallback(WriteCompleteCallback writeCompleteCallback) {
    writeCompleteCallback_ = std::move(writeCompleteCallback);
  }

  [[nodiscard]] std::span<const std::byte> inputView() const noexcept {
    return io_.inputView();
  }

  [[nodiscard]] bool peerClosed() const noexcept {
    return io_.peerHalfClosed();
  }

  [[nodiscard]] std::size_t pendingBytes() const noexcept {
    return io_.pendingBytes();
  }

  // Owner teardown: unregister without notifying a possibly destructing owner.
  void stop() noexcept;

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
  MessageCallback messageCallback_;
  const Identity identity_;
  CloseCallback closeCallback_;
  Channel connectionChannel_;

  bool inputStopped_{false};
  bool draining_{false};
  bool readPaused_{false};
  WriteCompleteCallback writeCompleteCallback_;

  bool handlingEvent_{false};
  bool eofNotified_{false};
  std::size_t messageCount_{0};
  State state_{State::kUnregistered};

  // Intrusive owner recovery record: no allocation when an event requests
  // close.
  TcpConnection* nextClosing_{nullptr};
  bool queuedForRecovery_{false};

  std::size_t eventCount_{0};
  std::uint32_t lastMask_{0};
  ConnectionEventResult lastResult_;
};
}  // namespace hp::net
