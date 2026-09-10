#include "net/tcp_server.h"

#include <stdexcept>
#include <utility>

#include "base/logger.h"

namespace hp::net {
TcpServer::TcpServer(std::uint16_t requested_port,
                     MessageCallbackFactory factory,
                     std::size_t max_input_bytes, std::size_t worker_count,
                     ConnectionTimeouts timeouts)
    : callback_factory_(std::move(factory)),
      max_input_bytes_(max_input_bytes),
      worker_count_(worker_count),
      timeouts_(timeouts),
      registries_(worker_count <= 64 ? worker_count : 0),
      acceptor_(loop_, requested_port,
                [this](Socket socket) { add_connection(std::move(socket)); }) {
  if (timeouts.idle.count() < 0 || timeouts.keep_alive.count() < 0 ||
      timeouts.idle.count() > 86400000 ||
      timeouts.keep_alive.count() > 86400000)
    throw std::invalid_argument("timeout outside 0-86400000ms");
  if (worker_count > 64) throw std::invalid_argument("worker count exceeds 64");
  loop_.set_control_callback(
      [this](EventLoop::Control kind, EventLoop::Deadline deadline) {
        control(kind, deadline);
      });
  if (worker_count == 0) {
    main_registry_ = std::make_unique<ConnectionRegistry>(
        loop_, max_input_bytes_, timeouts_);
    main_registry_->set_drained_callback([this] { loop_.request_stop(); });
  } else {
    pool_.start(
        worker_count,
        [this](std::size_t index, EventLoop& loop) {
          registries_[index] = std::make_unique<ConnectionRegistry>(
              loop, max_input_bytes_, timeouts_);
          registries_[index]->set_drained_callback(
              [&loop] { loop.request_stop(); });
          loop.set_control_callback(
              [this, index](EventLoop::Control kind, EventLoop::Deadline) {
                if (kind != EventLoop::Control::none)
                  registries_[index]->begin_drain(kind ==
                                                  EventLoop::Control::force);
              });
        },
        [this](std::size_t index, EventLoop& worker) {
          if (worker.failed() || !stopping_.exchange(true)) {
            worker_failed_ = true;
            stopping_ = true;
            loop_.request_force();
          }
          registries_[index].reset();
          ++workers_finished_;
          loop_.notify_control();
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
  if (control_channel_) {
    control_channel_->remove();
    control_channel_.reset();
  }
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

void TcpServer::watch_control_fd(int fd, Channel::Callback callback) {
  if (!loop_.is_in_loop_thread())
    throw std::logic_error("control fd attachment requires main owner");
  if (control_channel_) throw std::logic_error("control fd already attached");
  auto channel = std::make_unique<Channel>(loop_, fd, std::move(callback));
  channel->set_interest(EPOLLIN);
  control_channel_ = std::move(channel);
}

void TcpServer::request_graceful_shutdown(EventLoop::Deadline deadline) {
  stopping_ = true;  // Linearize handoff rejection before notifying any owner.
  loop_.request_drain(deadline);
}

void TcpServer::force_shutdown() {
  stopping_ = true;
  loop_.request_force();
}

void TcpServer::control(EventLoop::Control kind, EventLoop::Deadline deadline) {
  if (kind == EventLoop::Control::none) return;
  acceptor_.close();
  if (kind == EventLoop::Control::force) {
    draining_ = true;
    if (worker_count_)
      pool_.request_force();
    else
      main_registry_->begin_drain(true);
  } else if (!draining_) {
    draining_ = true;
    if (worker_count_)
      pool_.request_drain(deadline);
    else
      main_registry_->begin_drain();
  }
  if (worker_count_ && workers_finished_.load() == worker_count_)
    loop_.request_stop();
}

void TcpServer::request_stop() {
  stopping_ = true;
  loop_.request_stop();
}

void TcpServer::shutdown() {
  stopping_ = true;
  acceptor_.close();
  pool_.request_stop();
  std::exception_ptr error;
  try {
    pool_.join();
  } catch (...) {
    error = std::current_exception();
  }
  main_registry_.reset();
  if (error) std::rethrow_exception(error);
}

void TcpServer::run() {
  if (ran_) throw std::logic_error("TcpServer cannot restart");
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
    if (!error) error = std::current_exception();
  }
  if (error) std::rethrow_exception(error);
  if (worker_failed_) throw std::runtime_error("worker stopped unexpectedly");
}

void TcpServer::add_connection(Socket socket) {
  if (stopping_) {
    acceptor_.stop();
    return;
  }
  auto callback = callback_factory_ ? callback_factory_()
                                    : TcpConnection::MessageCallback{};
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

  auto handoff = std::make_shared<Handoff>(
      Handoff{std::move(socket), std::move(callback)});
  pool_.post(index, [this, index, handoff](EventLoop&) {
    if (stopping_) return;
    try {
      registries_[index]->add(std::move(handoff->socket),
                              std::move(handoff->callback));
    } catch (const std::bad_alloc&) {
      throw;
    } catch (const std::exception& error) {
      base::warn(error.what());
    } catch (...) {
      base::warn("connection adoption failed");
    }
  });
}
}  // namespace hp::net
