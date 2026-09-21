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

Acceptor::Acceptor(EventLoop& loop, std::uint16_t port)
    : listener_(createListener(port)),
      boundPort_(listener_.localPort()),
      listenerChannel_(loop, listener_.fd()) {
  listenerChannel_.registerEventCallback(
      [this](std::uint32_t events) { handleListenerEvent(events); });
}

Acceptor::~Acceptor() noexcept { stop(); }

void Acceptor::start() {
  if (!acceptedCallback_)
    throw std::invalid_argument("missing accepted callback");
  listenerChannel_.setInterest(EPOLLIN);
}

void Acceptor::close() noexcept {
  stop();
  listener_.reset();
}

void Acceptor::stop() noexcept { listenerChannel_.remove(); }

void Acceptor::handleListenerEvent(std::uint32_t mask) {
  if (mask & EPOLLIN) acceptReady();
  if (mask & (EPOLLERR | EPOLLHUP)) {
    const int error = listener_.socketError();
    throw std::system_error(error == 0 ? EIO : error,
                            std::generic_category(),
                            "listener epoll event");
  }
}

void Acceptor::acceptReady() {
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
      acceptedCallback_(std::move(accepted));
    } catch (...) {
      // Delivery owns a by-value Socket, so unwinding closes only that fd.
      try {
        base::warn("accepted connection delivery failed");
      } catch (...) {
      }
    }
  }
}
}  // namespace hp::net
