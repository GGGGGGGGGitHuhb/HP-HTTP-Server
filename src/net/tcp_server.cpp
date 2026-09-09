#include "net/tcp_server.h"
#include "base/logger.h"
#include <stdexcept>
#include <utility>

namespace hp::net {
TcpServer::TcpServer(std::uint16_t requested_port, MessageCallbackFactory factory,
                     std::size_t max_input_bytes, std::size_t worker_count)
    : callback_factory_(std::move(factory)), max_input_bytes_(max_input_bytes),
      worker_count_(worker_count), registries_(worker_count <= 64 ? worker_count : 0),
      acceptor_(loop_, requested_port, [this](Socket socket) {
          add_connection(std::move(socket));
      }) {
    if (worker_count > 64)
        throw std::invalid_argument("worker count exceeds 64");
    if (worker_count == 0) {
        main_registry_ = std::make_unique<ConnectionRegistry>(loop_, max_input_bytes_);
    } else {
        pool_.start(
            worker_count,
            [this](std::size_t index, EventLoop& loop) {
                registries_[index] = std::make_unique<ConnectionRegistry>(loop, max_input_bytes_);
            },
            [this](std::size_t index, EventLoop&) {
                if (!stopping_.exchange(true)) {
                    worker_failed_ = true;
                    loop_.request_stop();
                }
                registries_[index].reset();
            });
    }
    try {
        acceptor_.start();
    } catch (...) {
        auto error = std::current_exception();
        try {
            shutdown();
        } catch (...) {
        }
        std::rethrow_exception(error);
    }
}

TcpServer::~TcpServer() noexcept {
    try {
        shutdown();
    } catch (const std::exception& error) {
        base::error(error.what());
    } catch (...) {
        base::error("unobserved TcpServer failure");
    }
}

std::uint16_t TcpServer::bound_port() const noexcept {
    return acceptor_.bound_port();
}

void TcpServer::request_stop() {
    stopping_ = true;
    loop_.request_stop();
}

void TcpServer::shutdown() {
    stopping_ = true;
    acceptor_.stop();
    pool_.request_stop();
    std::exception_ptr error;
    try {
        pool_.join();
    } catch (...) {
        error = std::current_exception();
    }
    main_registry_.reset();
    if (error)
        std::rethrow_exception(error);
}

void TcpServer::run() {
    if (ran_)
        throw std::logic_error("TcpServer cannot restart");
    ran_ = true;
    std::exception_ptr error;
    try {
        loop_.loop();
    } catch (...) {
        error = std::current_exception();
    }
    try {
        shutdown();
    } catch (...) {
        if (!error)
            error = std::current_exception();
    }
    if (error)
        std::rethrow_exception(error);
    if (worker_failed_)
        throw std::runtime_error("worker stopped unexpectedly");
}

void TcpServer::add_connection(Socket socket) {
    if (stopping_) {
        acceptor_.stop();
        return;
    }
    auto callback = callback_factory_ ? callback_factory_() : TcpConnection::MessageCallback{};
    if (worker_count_ == 0) {
        main_registry_->add(std::move(socket), std::move(callback));
        return;
    }
    const auto index = next_worker_;
    next_worker_ = (next_worker_ + 1) % worker_count_;

    struct Handoff {
        Socket socket;
        TcpConnection::MessageCallback callback;
    };

    auto handoff = std::make_shared<Handoff>(Handoff{std::move(socket), std::move(callback)});
    pool_.post(index, [this, index, handoff](EventLoop&) {
        if (stopping_)
            return;
        try {
            registries_[index]->add(std::move(handoff->socket), std::move(handoff->callback));
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const std::exception& error) {
            base::warn(error.what());
        } catch (...) {
            base::warn("connection adoption failed");
        }
    });
}
} // namespace hp::net
