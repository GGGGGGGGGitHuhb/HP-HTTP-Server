#pragma once
#include "net/channel.h"
#include "net/socket.h"

namespace hp::net {
struct AcceptorTestAccess;

// Single loop thread. The receiver takes sole ownership of each accepted
// Socket.
class Acceptor final : private base::NonCopyable {
 public:
  using AcceptedCallback = std::function<void(Socket)>;

  Acceptor(EventLoop& loop, std::uint16_t port);
  void set_AddConnection_callback(
      AcceptedCallback accepted_connection_callback);
  ~Acceptor() noexcept;

  void Start();
  void Stop() noexcept;
  void Close() noexcept;

  [[nodiscard]] std::uint16_t bound_port() const noexcept {
    return bound_port_;
  }

 private:
  friend struct AcceptorTestAccess;

  static Socket CreateListener(std::uint16_t port);
  void HandleListenerEvent(std::uint32_t mask);
  void AcceptReady();

  Socket listener_;
  AcceptedCallback accepted_connection_callback_;
  const std::uint16_t bound_port_;

  Channel listener_channel_;
};
}  // namespace hp::net
