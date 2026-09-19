#include "net/tcp_server.h"

#include <stdexcept>
#include <utility>

#include "base/logger.h"

namespace hp::net {
TcpServer::TcpServer(std::uint16_t requested_port,
                     std::size_t max_input_bytes,
                     std::size_t worker_count,
                     ConnectionTimeouts timeouts)
    : max_input_bytes_(max_input_bytes),
      worker_count_(worker_count),
      timeouts_(timeouts),
      registries_(worker_count <= 64 ? worker_count : 0),
      acceptor_(loop_, requested_port) {
  acceptor_.set_AddConnection_callback(
      std::bind_front(&TcpServer::AddConnection, this));
  if (timeouts.idle.count() < 0 || timeouts.keep_alive.count() < 0 ||
      timeouts.idle.count() > 86400000 ||
      timeouts.keep_alive.count() > 86400000)
    throw std::invalid_argument("timeout outside 0-86400000ms");
  if (worker_count > 64) throw std::invalid_argument("worker count exceeds 64");
  loop_.set_HandleControl_callback(
      std::bind_front(&TcpServer::HandleControl, this));
  if (worker_count == 0) {
    main_registry_ = std::make_unique<ConnectionRegistry>(loop_,
                                                          max_input_bytes_,
                                                          timeouts_);
    main_registry_->set_RequestStop_callback(
        std::bind_front(&EventLoop::RequestStop, &loop_));
  } else {
    pool_.Start(worker_count,
                std::bind_front(&TcpServer::InitializeWorkerRegistry, this),
                std::bind_front(&TcpServer::CleanupWorkerRegistry, this));
  }
}

void TcpServer::set_CreateMessageCallback_callback(
    MessageCallbackFactory message_callback_factory) {
  if (ran_) throw std::logic_error("cannot replace running server factory");
  message_callback_factory_ = std::move(message_callback_factory);
}

TcpServer::~TcpServer() noexcept {
  if (control_channel_) {
    control_channel_->Remove();
    control_channel_.reset();
  }
  try {
    Shutdown();
  } catch (const std::exception& error) {
    base::error(error.what());
  } catch (...) {
    base::error("unobserved TcpServer failure");
  }
}

std::uint16_t TcpServer::bound_port() const noexcept {
  return acceptor_.bound_port();
}

Channel& TcpServer::WatchControlFd(int fd) {
  if (!loop_.is_in_loop_thread())
    throw std::logic_error("control fd attachment requires main owner");
  if (control_channel_) throw std::logic_error("control fd already attached");
  control_channel_ = std::make_unique<Channel>(loop_, fd);
  return *control_channel_;
}

void TcpServer::RequestGracefulShutdown(EventLoop::Deadline deadline) {
  stopping_ = true;  // Linearize handoff rejection before notifying any owner.
  loop_.RequestDrain(deadline);
}

void TcpServer::ForceShutdown() {
  stopping_ = true;
  loop_.RequestForce();
}

void TcpServer::HandleWorkerControl(std::size_t index,
                                    EventLoop::Control kind,
                                    EventLoop::Deadline) {
  if (kind != EventLoop::Control::kNone)
    registries_[index]->BeginDrain(kind == EventLoop::Control::kForce);
}

void TcpServer::HandleControl(EventLoop::Control kind,
                              EventLoop::Deadline deadline) {
  if (kind == EventLoop::Control::kNone) return;
  acceptor_.Close();
  if (kind == EventLoop::Control::kForce) {
    draining_ = true;
    if (worker_count_)
      pool_.RequestForce();
    else
      main_registry_->BeginDrain(true);
  } else if (!draining_) {
    draining_ = true;
    if (worker_count_)
      pool_.RequestDrain(deadline);
    else
      main_registry_->BeginDrain();
  }
  if (worker_count_ && workers_finished_.load() == worker_count_)
    loop_.RequestStop();
}

void TcpServer::RequestStop() {
  stopping_ = true;
  loop_.RequestStop();
}

void TcpServer::Shutdown() {
  stopping_ = true;
  acceptor_.Close();
  pool_.RequestStop();
  std::exception_ptr error;
  try {
    pool_.Join();
  } catch (...) {
    error = std::current_exception();
  }
  main_registry_.reset();
  if (error) std::rethrow_exception(error);
}

void TcpServer::Run() {
  if (ran_) throw std::logic_error("TcpServer cannot restart");
  ran_ = true;
  std::exception_ptr error;
  try {
    if (!stopping_) acceptor_.Start();
    loop_.Loop();
  } catch (...) {
    error = std::current_exception();
  }
  try {
    Shutdown();
  } catch (...) {
    if (!error) error = std::current_exception();
  }
  if (error) std::rethrow_exception(error);
  if (worker_failed_) throw std::runtime_error("worker stopped unexpectedly");
}

void TcpServer::AddConnection(Socket socket) {
  if (stopping_) {
    acceptor_.Stop();
    return;
  }
  auto message_callback = message_callback_factory_
                              ? message_callback_factory_()
                              : TcpConnection::MessageCallback{};
  if (worker_count_ == 0) {
    main_registry_->AddConnection(std::move(socket),
                                  std::move(message_callback));
    return;
  }
  const auto index = next_worker_;
  next_worker_ = (next_worker_ + 1) % worker_count_;

  auto handoff = std::make_shared<ConnectionHandoff>(
      ConnectionHandoff{std::move(socket), std::move(message_callback)});
  pool_.Post(
      index,
      std::bind_front(&TcpServer::AdoptConnection, this, index, handoff));
}

void TcpServer::InitializeWorkerRegistry(std::size_t index, EventLoop& loop) {
  registries_[index] =
      std::make_unique<ConnectionRegistry>(loop, max_input_bytes_, timeouts_);
  registries_[index]->set_RequestStop_callback(
      std::bind_front(&EventLoop::RequestStop, &loop));
  loop.set_HandleWorkerControl_callback(
      std::bind_front(&TcpServer::HandleWorkerControl, this, index));
}

void TcpServer::CleanupWorkerRegistry(std::size_t index, EventLoop& worker) {
  if (worker.failed() || !stopping_.exchange(true)) {
    worker_failed_ = true;
    stopping_ = true;
    loop_.RequestForce();
  }
  registries_[index].reset();
  ++workers_finished_;
  loop_.NotifyControl();
}

void TcpServer::AdoptConnection(
    std::size_t index,
    const std::shared_ptr<ConnectionHandoff>& handoff,
    EventLoop&) {
  if (stopping_) return;
  try {
    registries_[index]->AddConnection(std::move(handoff->socket),
                                      std::move(handoff->message_callback));
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    base::warn(error.what());
  } catch (...) {
    base::warn("connection adoption failed");
  }
}
}  // namespace hp::net
