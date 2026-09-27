#pragma once
#include "net/Channel.h"
#include "net/Socket.h"

namespace hp::net {
struct AcceptorTestAccess;

// 仅在单个事件循环线程使用。接收方独占每个已接受的
// Socket 的所有权。
class Acceptor final : private base::NonCopyable {
 public:
  using AcceptedCallback = std::function<void(Socket)>;

  Acceptor(EventLoop& ownerEventLoop, std::uint16_t port);
  void setAcceptedCallback(AcceptedCallback acceptedCallback) {
    acceptedCallback_ = std::move(acceptedCallback);
  }

  ~Acceptor() noexcept;

  void enableAcceptEvents();
  void disableAcceptEvents() noexcept;
  void closeListener() noexcept;

  [[nodiscard]] std::uint16_t boundPort() const noexcept { return boundPort_; }

 private:
  friend struct AcceptorTestAccess;

  static Socket createListener(std::uint16_t port);
  void handleListenerEvent(std::uint32_t mask);
  void acceptPendingConnections();

  Socket listener_;
  AcceptedCallback acceptedCallback_;  // 绑定 `TcpServer::onAccepted()`
  const std::uint16_t boundPort_;

  Channel listenerChannel_;
};
}  // namespace hp::net
