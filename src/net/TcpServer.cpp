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
      workerRegistries_(workerCount <= 64 ? workerCount : 0),
      acceptor_(mainEventLoop_, requestedPort) {
  acceptor_.setAcceptedCallback([this](Socket socket) { onAccepted(std::move(socket)); });
  if (timeouts.idle.count() < 0 || timeouts.keepAlive.count() < 0 ||
      timeouts.idle.count() > 86400000 || timeouts.keepAlive.count() > 86400000)
    throw std::invalid_argument("timeout outside 0-86400000ms");
  if (workerCount > 64) throw std::invalid_argument("worker count exceeds 64");
  mainEventLoop_.registerControlCallback(
      [this](EventLoop::Control control, EventLoop::Deadline deadline) {
        onControl(control, deadline);
      });
  if (workerCount == 0) {
    // 连接注册表属于主循环
    mainConnectionRegistry_ =
        std::make_unique<ConnectionRegistry>(mainEventLoop_, maxInputBytes_, timeouts_);
    mainConnectionRegistry_->setStopCallback(
        [target = &mainEventLoop_]() { target->requestLoopStop(); });
  } else {
    workerPool_.createWorkerThreads(
        workerCount,
        [this](std::size_t workerIndex, EventLoop& workerEventLoop) {
          initializeWorkerRegistry(workerIndex, workerEventLoop);
        },
        [this](std::size_t workerIndex, EventLoop& workerEventLoop) {
          cleanupWorkerRegistry(workerIndex, workerEventLoop);
        });
  }
}

void TcpServer::registerMessageFactoryCallback(MessageFactoryCallback messageFactoryCallback) {
  if (ran_) throw std::logic_error("cannot replace running server factory");
  setMessageFactoryCallback(std::move(messageFactoryCallback));
}

TcpServer::~TcpServer() noexcept {
  if (controlChannel_) {
    controlChannel_->removeChannel();
    controlChannel_.reset();
  }
  try {
    shutdownTcpServer();
  } catch (const std::exception& error) {
    base::error(error.what());
  } catch (...) {
    base::error("unobserved TcpServer failure");
  }
}

std::uint16_t TcpServer::boundPort() const noexcept { return acceptor_.boundPort(); }

Channel& TcpServer::watchControlFd(int fd) {
  if (!mainEventLoop_.isInLoopThread())
    throw std::logic_error("control fd attachment requires main owner");
  if (controlChannel_) throw std::logic_error("control fd already attached");
  controlChannel_ = std::make_unique<Channel>(mainEventLoop_, fd);
  return *controlChannel_;
}

void TcpServer::requestServerGracefulShutdown(EventLoop::Deadline deadline) {
  stopping_ = true;  // 在通知任何所属线程前，先确立拒绝连接移交的线性化点。
  mainEventLoop_.requestLoopDrain(deadline);
}

void TcpServer::requestServerForceClose() {
  stopping_ = true;
  mainEventLoop_.requestLoopForceClose();
}

void TcpServer::onControl(std::size_t workerIndex, EventLoop::Control kind, EventLoop::Deadline) {
  if (kind != EventLoop::Control::kNone)
    workerRegistries_[workerIndex]->beginConnectionsDrain(kind == EventLoop::Control::kForce);
}

void TcpServer::onControl(EventLoop::Control kind, EventLoop::Deadline deadline) {
  if (kind == EventLoop::Control::kNone) return;
  acceptor_.closeListener();
  if (kind == EventLoop::Control::kForce) {
    draining_ = true;
    if (workerCount_)
      workerPool_.requestWorkersForceClose();
    else
      mainConnectionRegistry_->beginConnectionsDrain(true);
  } else if (!draining_) {
    draining_ = true;
    if (workerCount_)
      workerPool_.requestWorkersDrain(deadline);
    else
      mainConnectionRegistry_->beginConnectionsDrain();
  }
  if (workerCount_ && workersFinished_.load() == workerCount_) mainEventLoop_.requestLoopStop();
}

void TcpServer::requestServerStop() {
  stopping_ = true;
  mainEventLoop_.requestLoopStop();
}

void TcpServer::shutdownTcpServer() {
  stopping_ = true;
  acceptor_.closeListener();
  workerPool_.requestPoolStop();
  std::exception_ptr error;
  try {
    workerPool_.joinWorkerThreads();
  } catch (...) {
    error = std::current_exception();
  }
  mainConnectionRegistry_.reset();
  if (error) std::rethrow_exception(error);
}

void TcpServer::runTcpServer() {
  if (ran_) throw std::logic_error("TcpServer cannot restart");
  ran_ = true;
  std::exception_ptr error;
  try {
    // 启动监听并运行主循环
    if (!stopping_) acceptor_.enableAcceptEvents();
    mainEventLoop_.runEventLoop();
  } catch (...) {
    error = std::current_exception();
  }
  try {
    shutdownTcpServer();
  } catch (...) {
    if (!error) error = std::current_exception();
  }
  if (error) std::rethrow_exception(error);
  if (workerFailed_) throw std::runtime_error("worker stopped unexpectedly");
}

void TcpServer::onAccepted(Socket socket) {
  if (stopping_) {
    acceptor_.disableAcceptEvents();
    return;
  }
  auto messageCallback =
      messageFactoryCallback_ ? messageFactoryCallback_() : TcpConnection::MessageCallback{};
  if (workerCount_ == 0) {
    mainConnectionRegistry_->onAccepted(std::move(socket), std::move(messageCallback));
    return;
  }
  const auto workerIndex = nextWorkerIndex_;
  nextWorkerIndex_ = (nextWorkerIndex_ + 1) % workerCount_;

  // 把 socket 和该连接的消息回调放入 `ConnectionHandoff` 并投递
  auto handoff = std::make_shared<ConnectionHandoff>(
      ConnectionHandoff{std::move(socket), std::move(messageCallback)});
  workerPool_.postTaskToWorkerAtIndex(workerIndex,
                                      [this, workerIndex, handoff](EventLoop& workerEventLoop) {
                                        adoptConnection(workerIndex, handoff, workerEventLoop);
                                      });
}

void TcpServer::initializeWorkerRegistry(std::size_t workerIndex, EventLoop& workerEventLoop) {
  workerRegistries_[workerIndex] =
      std::make_unique<ConnectionRegistry>(workerEventLoop, maxInputBytes_, timeouts_);
  workerRegistries_[workerIndex]->setStopCallback(
      [target = &workerEventLoop]() { target->requestLoopStop(); });
  workerEventLoop.registerControlCallback(
      [this, workerIndex](EventLoop::Control control, EventLoop::Deadline deadline) {
        onControl(workerIndex, control, deadline);
      });
}

void TcpServer::cleanupWorkerRegistry(std::size_t workerIndex, EventLoop& workerEventLoop) {
  if (workerEventLoop.failed() || !stopping_.exchange(true)) {
    workerFailed_ = true;
    stopping_ = true;
    mainEventLoop_.requestLoopForceClose();
  }
  workerRegistries_[workerIndex].reset();
  ++workersFinished_;
  mainEventLoop_.notifyControl();
}

void TcpServer::adoptConnection(std::size_t workerIndex,
                                const std::shared_ptr<ConnectionHandoff>& handoff,
                                EventLoop&) {
  if (stopping_) return;
  try {
    workerRegistries_[workerIndex]->onAccepted(std::move(handoff->socket),
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
