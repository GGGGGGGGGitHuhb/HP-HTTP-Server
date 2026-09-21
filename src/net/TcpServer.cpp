#include "net/TcpServer.h"

#include <stdexcept>
#include <utility>

#include "base/Logger.h"

namespace hp::net {
TcpServer::TcpServer(std::uint16_t requestedPort,
                     std::size_t maxInputBytes,
                     std::size_t workerCount,
                     ConnectionTimeouts timeouts)
    : maxInputBytes_(maxInputBytes),
      workerCount_(workerCount),
      timeouts_(timeouts),
      registries_(workerCount <= 64 ? workerCount : 0),
      acceptor_(loop_, requestedPort) {
  acceptor_.setAcceptedCallback(
      [this](Socket socket) { onAccepted(std::move(socket)); });
  if (timeouts.idle.count() < 0 || timeouts.keepAlive.count() < 0 ||
      timeouts.idle.count() > 86400000 || timeouts.keepAlive.count() > 86400000)
    throw std::invalid_argument("timeout outside 0-86400000ms");
  if (workerCount > 64) throw std::invalid_argument("worker count exceeds 64");
  loop_.registerControlCallback(
      [this](EventLoop::Control control, EventLoop::Deadline deadline) {
        onControl(control, deadline);
      });
  if (workerCount == 0) {
    mainRegistry_ =
        std::make_unique<ConnectionRegistry>(loop_, maxInputBytes_, timeouts_);
    mainRegistry_->setStopCallback(
        [target = &loop_]() { target->requestStop(); });
  } else {
    pool_.start(
        workerCount,
        [this](std::size_t index, EventLoop& loop) {
          initializeWorkerRegistry(index, loop);
        },
        [this](std::size_t index, EventLoop& loop) {
          cleanupWorkerRegistry(index, loop);
        });
  }
}

void TcpServer::registerMessageFactoryCallback(
    MessageFactoryCallback messageFactoryCallback) {
  if (ran_) throw std::logic_error("cannot replace running server factory");
  setMessageFactoryCallback(std::move(messageFactoryCallback));
}

TcpServer::~TcpServer() noexcept {
  if (controlChannel_) {
    controlChannel_->remove();
    controlChannel_.reset();
  }
  try {
    shutdown();
  } catch (const std::exception& error) {
    base::error(error.what());
  } catch (...) {
    base::error("unobserved TcpServer failure");
  }
}

std::uint16_t TcpServer::boundPort() const noexcept {
  return acceptor_.boundPort();
}

Channel& TcpServer::watchControlFd(int fd) {
  if (!loop_.isInLoopThread())
    throw std::logic_error("control fd attachment requires main owner");
  if (controlChannel_) throw std::logic_error("control fd already attached");
  controlChannel_ = std::make_unique<Channel>(loop_, fd);
  return *controlChannel_;
}

void TcpServer::requestGracefulShutdown(EventLoop::Deadline deadline) {
  stopping_ = true;  // Linearize handoff rejection before notifying any owner.
  loop_.requestDrain(deadline);
}

void TcpServer::forceShutdown() {
  stopping_ = true;
  loop_.requestForce();
}

void TcpServer::onControl(std::size_t index,
                          EventLoop::Control kind,
                          EventLoop::Deadline) {
  if (kind != EventLoop::Control::kNone)
    registries_[index]->beginDrain(kind == EventLoop::Control::kForce);
}

void TcpServer::onControl(EventLoop::Control kind,
                          EventLoop::Deadline deadline) {
  if (kind == EventLoop::Control::kNone) return;
  acceptor_.close();
  if (kind == EventLoop::Control::kForce) {
    draining_ = true;
    if (workerCount_)
      pool_.requestForce();
    else
      mainRegistry_->beginDrain(true);
  } else if (!draining_) {
    draining_ = true;
    if (workerCount_)
      pool_.requestDrain(deadline);
    else
      mainRegistry_->beginDrain();
  }
  if (workerCount_ && workersFinished_.load() == workerCount_)
    loop_.requestStop();
}

void TcpServer::requestStop() {
  stopping_ = true;
  loop_.requestStop();
}

void TcpServer::shutdown() {
  stopping_ = true;
  acceptor_.close();
  pool_.requestStop();
  std::exception_ptr error;
  try {
    pool_.join();
  } catch (...) {
    error = std::current_exception();
  }
  mainRegistry_.reset();
  if (error) std::rethrow_exception(error);
}

void TcpServer::run() {
  if (ran_) throw std::logic_error("TcpServer cannot restart");
  ran_ = true;
  std::exception_ptr error;
  try {
    if (!stopping_) acceptor_.start();
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
  if (workerFailed_) throw std::runtime_error("worker stopped unexpectedly");
}

void TcpServer::onAccepted(Socket socket) {
  if (stopping_) {
    acceptor_.stop();
    return;
  }
  auto messageCallback = messageFactoryCallback_
                             ? messageFactoryCallback_()
                             : TcpConnection::MessageCallback{};
  if (workerCount_ == 0) {
    mainRegistry_->onAccepted(std::move(socket), std::move(messageCallback));
    return;
  }
  const auto index = nextWorker_;
  nextWorker_ = (nextWorker_ + 1) % workerCount_;

  auto handoff = std::make_shared<ConnectionHandoff>(
      ConnectionHandoff{std::move(socket), std::move(messageCallback)});
  pool_.post(index, [this, index, handoff](EventLoop& loop) {
    adoptConnection(index, handoff, loop);
  });
}

void TcpServer::initializeWorkerRegistry(std::size_t index, EventLoop& loop) {
  registries_[index] =
      std::make_unique<ConnectionRegistry>(loop, maxInputBytes_, timeouts_);
  registries_[index]->setStopCallback(
      [target = &loop]() { target->requestStop(); });
  loop.registerControlCallback(
      [this, index](EventLoop::Control control, EventLoop::Deadline deadline) {
        onControl(index, control, deadline);
      });
}

void TcpServer::cleanupWorkerRegistry(std::size_t index, EventLoop& worker) {
  if (worker.failed() || !stopping_.exchange(true)) {
    workerFailed_ = true;
    stopping_ = true;
    loop_.requestForce();
  }
  registries_[index].reset();
  ++workersFinished_;
  loop_.notifyControl();
}

void TcpServer::adoptConnection(
    std::size_t index,
    const std::shared_ptr<ConnectionHandoff>& handoff,
    EventLoop&) {
  if (stopping_) return;
  try {
    registries_[index]->onAccepted(std::move(handoff->socket),
                                   std::move(handoff->messageCallback));
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    base::warn(error.what());
  } catch (...) {
    base::warn("connection adoption failed");
  }
}
}  // namespace hp::net
