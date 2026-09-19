#include "net/acceptor.h"

#include <cerrno>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include "base/logger.h"
#include "net/event_loop.h"

namespace hp::net {
Socket Acceptor::CreateListener(std::uint16_t port) {
  Socket listener = Socket::CreateTcp();
  listener.set_reuse_address(true);
  listener.BindAny(port);
  listener.Listen(128);
  return listener;
}

Acceptor::Acceptor(EventLoop& loop, std::uint16_t port)
    : listener_(CreateListener(port)),
      bound_port_(listener_.local_port()),
      listener_channel_(loop, listener_.fd()) {
  listener_channel_.set_HandleListenerEvent_callback(
      std::bind_front(&Acceptor::HandleListenerEvent, this));
}

void Acceptor::set_AddConnection_callback(
    AcceptedCallback accepted_connection_callback) {
  accepted_connection_callback_ = std::move(accepted_connection_callback);
}

Acceptor::~Acceptor() noexcept { Stop(); }

void Acceptor::Start() {
  if (!accepted_connection_callback_)
    throw std::invalid_argument("missing accepted callback");
  listener_channel_.set_interest(EPOLLIN);
}

void Acceptor::Close() noexcept {
  Stop();
  listener_.Reset();
}

void Acceptor::Stop() noexcept { listener_channel_.Remove(); }

void Acceptor::HandleListenerEvent(std::uint32_t mask) {
  if (mask & EPOLLIN) AcceptReady();
  if (mask & (EPOLLERR | EPOLLHUP)) {
    const int error = listener_.socket_error();
    throw std::system_error(error == 0 ? EIO : error,
                            std::generic_category(),
                            "listener epoll event");
  }
}

void Acceptor::AcceptReady() {
  while (listener_channel_.registered()) {
    Socket accepted;
    try {
      accepted = listener_.AcceptNonBlocking();
    } catch (const std::system_error& error) {
      try {
        base::warn(std::string("accept4 failed: ") + error.what());
      } catch (...) {
      }
      return;
    }
    if (!accepted.valid()) return;
    try {
      accepted_connection_callback_(std::move(accepted));
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
