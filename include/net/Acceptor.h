#pragma once
#include "net/Channel.h"
#include "net/Socket.h"

namespace hp::net {
struct AcceptorTestAccess;

// Single loop thread. The receiver takes sole ownership of each accepted
// Socket.
class Acceptor final : private base::NonCopyable {
 public:
  using AcceptedCallback = std::function<void(Socket)>;

  Acceptor(EventLoop& loop, std::uint16_t port);
  void setAcceptedCallback(AcceptedCallback acceptedCallback) {
    acceptedCallback_ = std::move(acceptedCallback);
  }

  ~Acceptor() noexcept;

  void start();
  void stop() noexcept;
  void close() noexcept;

  [[nodiscard]] std::uint16_t boundPort() const noexcept { return boundPort_; }

 private:
  friend struct AcceptorTestAccess;

  static Socket createListener(std::uint16_t port);
  void handleListenerEvent(std::uint32_t mask);
  void acceptReady();

  Socket listener_;
  AcceptedCallback acceptedCallback_;
  const std::uint16_t boundPort_;

  Channel listenerChannel_;
};
}  // namespace hp::net
