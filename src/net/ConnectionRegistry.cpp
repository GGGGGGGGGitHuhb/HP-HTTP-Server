#include "net/ConnectionRegistry.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace hp::net {
ConnectionRegistry::ConnectionRegistry(EventLoop& loop,
                                       std::size_t maxInputBytes,
                                       ConnectionTimeouts timeouts)
    : timeouts_(timeouts), loop_(loop), maxInputBytes_(maxInputBytes) {
  loop_.registerCleanupCallback([this]() { onCleanup(); });
}

ConnectionRegistry::~ConnectionRegistry() noexcept {
  for (auto& [fd, connection] : connections_) {
    (void)fd;
    connection->stop();
  }
  connections_.clear();
  loop_.registerCleanupCallback({});
}

void ConnectionRegistry::beginDrain(bool force) {
  draining_ = true;
  for (auto& [fd, connection] : connections_) {
    (void)fd;
    cancelTimeout(*connection);
    if (force)
      connection->requestClose();
    else
      connection->beginDrain();
  }
  onCleanup();
}

void ConnectionRegistry::onAccepted(
    Socket socket,
    TcpConnection::MessageCallback messageCallback) {
  if (draining_) return;
  if (nextIdentity_ == 0)
    throw std::overflow_error("connection identity exhausted");
  const auto identity = nextIdentity_;
  nextIdentity_ =
      identity == std::numeric_limits<TcpConnection::Identity>::max()
          ? 0
          : identity + 1;
  const int fd = socket.fd();
  auto connection = std::make_unique<TcpConnection>(loop_,
                                                    std::move(socket),
                                                    identity,
                                                    maxInputBytes_);
  connection->setMessageCallback(std::move(messageCallback));
  connection->setCloseCallback(
      [this](int fd, TcpConnection::Identity identity) {
        onClose(fd, identity);
      });
  connection->setTimeoutActivityCallback(
      [this](TcpConnection& connection, bool progress) {
        onTimeoutActivity(connection, progress);
      });
  auto [position, inserted] =
      connections_.try_emplace(fd, std::move(connection));
  if (!inserted) throw std::logic_error("duplicate connection fd");
  try {
    position->second->start();
    position->second->lastProgress_ = timer::TimerQueue::Clock::now();
    onTimeoutActivity(*position->second, false);
  } catch (...) {
    position->second->stop();
    onCleanup();
    connections_.erase(fd);
    throw;
  }
}

void ConnectionRegistry::cancelTimeout(TcpConnection& connection) noexcept {
  if (connection.timeoutId_) {
    loop_.cancelTimer(connection.timeoutId_);
    connection.timeoutId_ = 0;
  }
}

void ConnectionRegistry::onTimeoutActivity(TcpConnection& connection,
                                           bool progress) {
  if (draining_ || connection.state() == TcpConnection::State::kClosing) {
    cancelTimeout(connection);
    return;
  }
  const auto now = timer::TimerQueue::Clock::now();
  if (progress) connection.lastProgress_ = now;
  if (!connection.idleWaiting_)
    connection.waitSince_.reset();
  else if (!connection.waitSince_)
    connection.waitSince_ = now;
  std::optional<timer::TimerQueue::TimePoint> deadline;
  if (timeouts_.idle.count())
    deadline = connection.lastProgress_ + timeouts_.idle;
  if (timeouts_.keepAlive.count() && connection.waitSince_) {
    const auto keepDeadline = *connection.waitSince_ + timeouts_.keepAlive;
    if (!deadline || keepDeadline < *deadline) deadline = keepDeadline;
  }
  if (!deadline) {
    cancelTimeout(connection);
    return;
  }
  try {
    if (connection.timeoutId_) {
      if (!loop_.rescheduleTimer(connection.timeoutId_, *deadline))
        connection.requestClose();
    } else {
      connection.timeoutId_ = loop_.addTimer(
          *deadline,
          [this, fd = connection.fd(), identity = connection.identity()]() {
            expireConnection(fd, identity);
          });
    }
  } catch (...) {
    connection.requestClose();
    throw;
  }
}

void ConnectionRegistry::expireConnection(int fd,
                                          TcpConnection::Identity identity) {
  if (draining_) return;
  const auto found = connections_.find(fd);
  if (found == connections_.end() || found->second->identity() != identity)
    return;
  found->second->timeoutId_ = 0;
  found->second->requestClose();
}

void ConnectionRegistry::onClose(int fd,
                                 TcpConnection::Identity identity) noexcept {
  const auto found = connections_.find(fd);
  if (found == connections_.end() || found->second->identity() != identity)
    return;
  auto& connection = *found->second;
  if (connection.queuedForRecovery_) return;
  connection.queuedForRecovery_ = true;
  connection.nextClosing_ = closingHead_;
  closingHead_ = &connection;
}

void ConnectionRegistry::onCleanup() noexcept {
  while (closingHead_) {
    auto* connection = closingHead_;
    closingHead_ = connection->nextClosing_;
    const auto found = connections_.find(connection->fd());
    if (found != connections_.end() &&
        found->second->identity() == connection->identity())
      connections_.erase(found);
  }
  if (draining_ && connections_.empty() && !notified_) {
    notified_ = true;
    if (stopCallback_) stopCallback_();
  }
}
}  // namespace hp::net
