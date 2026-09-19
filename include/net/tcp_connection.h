#pragma once
#include <chrono>
#include <optional>

#include "net/channel.h"
#include "net/connection_io.h"

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
  using CloseCallback = std::function<void(int, Identity)>;
  enum class State { kUnregistered, kActive, kClosing };

  TcpConnection(EventLoop& loop,
                Socket socket,
                Identity identity,
                std::size_t max_input_bytes);
  void set_HandleMessage_callback(MessageCallback message_callback);
  void set_OnConnectionClosed_callback(CloseCallback close_callback);
  void set_UpdateTimeout_callback(
      std::function<void(TcpConnection&, bool)> timeout_activity_callback);
  ~TcpConnection() noexcept;

  void Start();
  void RequestClose() noexcept;

  void Send(std::span<const std::byte> bytes);
  void SendFile(std::span<const std::byte> header, base::FileRegion file);
  void Consume(std::size_t count);

  void CloseAfterFlush();
  void BeginDrain();

  void PauseReading();
  void ResumeReading();

  void set_idle_wait(bool waiting);

  using WriteCompleteCallback = std::function<void(TcpConnection&)>;

  void set_HandleWriteComplete_callback(
      WriteCompleteCallback write_complete_callback);

  [[nodiscard]] std::span<const std::byte> input_view() const noexcept {
    return io_.input_view();
  }

  [[nodiscard]] bool peer_closed() const noexcept {
    return io_.peer_half_closed();
  }

  [[nodiscard]] std::size_t pending_bytes() const noexcept {
    return io_.pending_bytes();
  }

  // Owner teardown: unregister without notifying a possibly destructing owner.
  void Stop() noexcept;

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

  static void HandleEchoMessage(TcpConnection& connection,
                                std::span<const std::byte> input,
                                bool eof);
  void HandleConnectionEvent(std::uint32_t mask) noexcept;
  void UpdateInterest();

  void ReadMessages();
  void FlushOutput();

  std::function<void(TcpConnection&, bool)> timeout_activity_callback_;
  bool idle_waiting_{false};
  std::chrono::steady_clock::time_point last_progress_{};
  std::optional<std::chrono::steady_clock::time_point> wait_since_;
  std::uint64_t timeout_id_{0};

  ConnectionIo io_;
  MessageCallback message_callback_;
  const Identity identity_;
  CloseCallback close_callback_;
  Channel connection_channel_;

  bool input_stopped_{false};
  bool draining_{false};
  bool read_paused_{false};
  WriteCompleteCallback write_complete_callback_;

  bool handling_event_{false};
  bool eof_notified_{false};
  std::size_t message_count_{0};
  State state_{State::kUnregistered};

  // Intrusive owner recovery record: no allocation when an event requests
  // close.
  TcpConnection* next_closing_{nullptr};
  bool queued_for_recovery_{false};

  std::size_t event_count_{0};
  std::uint32_t last_mask_{0};
  ConnectionEventResult last_result_;
};
}  // namespace hp::net
