#include "net/acceptor.h"
#include "net/event_loop.h"
#include "base/logger.h"
#include <cerrno>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace hp::net {
Socket Acceptor::create_listener(std::uint16_t port) {
    Socket listener = Socket::create_tcp();
    listener.set_reuse_address(true);
    listener.bind_any(port);
    listener.listen(128);
    return listener;
}

Acceptor::Acceptor(EventLoop& loop, std::uint16_t port,
                   AcceptedCallback callback)
    : listener_(create_listener(port)),
      callback_(std::move(callback)),
      bound_port_(listener_.local_port()),
      channel_(loop, listener_.fd(),
               [this](std::uint32_t mask) { handle_event(mask); }) {
    if (!callback_)
        throw std::invalid_argument("missing accepted callback");
}

Acceptor::~Acceptor() noexcept {
    stop();
}

void Acceptor::start() {
    channel_.set_interest(EPOLLIN);
}

void Acceptor::close() noexcept {
    stop();
    listener_.reset();
}

void Acceptor::stop() noexcept {
    channel_.remove();
}

void Acceptor::handle_event(std::uint32_t mask) {
    if (mask & EPOLLIN)
        accept_ready();
    if (mask & (EPOLLERR | EPOLLHUP)) {
        const int error = listener_.socket_error();
        throw std::system_error(error == 0 ? EIO : error,
                                std::generic_category(),
                                "listener epoll event");
    }
}

void Acceptor::accept_ready() {
    while (channel_.registered()) {
        Socket accepted;
        try {
            accepted = listener_.accept_non_blocking();
        } catch (const std::system_error& error) {
            try {
                base::warn(std::string("accept4 failed: ") + error.what());
            } catch (...) {
            }
            return;
        }
        if (!accepted.valid())
            return;
        try {
            callback_(std::move(accepted));
        } catch (...) {
            // Delivery owns a by-value Socket, so unwinding closes only that fd.
            try {
                base::warn("accepted connection delivery failed");
            } catch (...) {
            }
        }
    }
}
}
