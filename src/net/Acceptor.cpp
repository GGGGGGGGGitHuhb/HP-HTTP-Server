#include "net/Acceptor.h"

#include <cerrno>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include "base/Logger.h"
#include "net/EventLoop.h"

namespace hp::net {
Socket Acceptor::createListener(std::uint16_t port) {
  Socket listener = Socket::createTcp();
  listener.setReuseAddress(true);
  listener.bindAny(port);
  listener.listen(128);
  return listener;
}

Acceptor::Acceptor(EventLoop& ownerEventLoop, std::uint16_t port)
    : listener_(createListener(port)),
      boundPort_(listener_.localPort()),
      listenerChannel_(ownerEventLoop, listener_.fd()) {
  listenerChannel_.registerEventCallback(
      [this](std::uint32_t events) { handleListenerEvent(events); });
}

Acceptor::~Acceptor() noexcept { disableAcceptEvents(); }

void Acceptor::enableAcceptEvents() {
  if (!acceptedCallback_) throw std::invalid_argument("missing accepted callback");
  listenerChannel_.setInterest(EPOLLIN);
}

void Acceptor::closeListener() noexcept {
  disableAcceptEvents();
  listener_.reset();
}

void Acceptor::disableAcceptEvents() noexcept { listenerChannel_.removeChannel(); }

void Acceptor::handleListenerEvent(std::uint32_t mask) {
  if (mask & EPOLLIN) acceptPendingConnections();
  if (mask & (EPOLLERR | EPOLLHUP)) {
    const int error = listener_.socketError();
    throw std::system_error(error == 0 ? EIO : error,
                            std::generic_category(),
                            "listener epoll event");
  }
}

void Acceptor::acceptPendingConnections() {
  while (listenerChannel_.registered()) {
    Socket accepted;
    try {
      accepted = listener_.acceptNonBlocking();
    } catch (const std::system_error& error) {
      try {
        base::warn(std::string("accept4 failed: ") + error.what());
      } catch (...) {
      }
      return;
    }
    if (!accepted.valid()) return;
    try {
      accepted.setTcpNoDelay(true);
      acceptedCallback_(std::move(accepted));
    } catch (...) {
      // 交付过程按值拥有 Socket，因此栈展开只关闭该 fd。
      try {
        base::warn("accepted connection delivery failed");
      } catch (...) {
      }
    }
  }
}
}  // namespace hp::net
