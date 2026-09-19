#include "net/connection_registry.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace hp::net {
ConnectionRegistry::ConnectionRegistry(EventLoop& loop,
                                       std::size_t max_input_bytes,
                                       ConnectionTimeouts timeouts)
    : timeouts_(timeouts), loop_(loop), max_input_bytes_(max_input_bytes) {
  loop_.set_DrainClosedConnections_callback(
      std::bind_front(&ConnectionRegistry::DrainClosedConnections, this));
}

ConnectionRegistry::~ConnectionRegistry() noexcept {
  for (auto& [fd, connection] : connections_) {
    (void)fd;
    connection->Stop();
  }
  connections_.clear();
  loop_.set_DrainClosedConnections_callback({});
}

void ConnectionRegistry::set_RequestStop_callback(
    EventLoop::LoopTask request_stop_callback) {
  request_stop_callback_ = std::move(request_stop_callback);
}

void ConnectionRegistry::BeginDrain(bool force) {
  draining_ = true;
  for (auto& [fd, connection] : connections_) {
    (void)fd;
    CancelTimeout(*connection);
    if (force)
      connection->RequestClose();
    else
      connection->BeginDrain();
  }
  DrainClosedConnections();
}

void ConnectionRegistry::AddConnection(
    Socket socket,
    TcpConnection::MessageCallback message_callback) {
  if (draining_) return;
  if (next_identity_ == 0)
    throw std::overflow_error("connection identity exhausted");
  const auto identity = next_identity_;
  next_identity_ =
      identity == std::numeric_limits<TcpConnection::Identity>::max()
          ? 0
          : identity + 1;
  const int fd = socket.fd();
  auto connection = std::make_unique<TcpConnection>(loop_,
                                                    std::move(socket),
                                                    identity,
                                                    max_input_bytes_);
  connection->set_HandleMessage_callback(std::move(message_callback));
  connection->set_OnConnectionClosed_callback(
      std::bind_front(&ConnectionRegistry::OnConnectionClosed, this));
  connection->set_UpdateTimeout_callback(
      std::bind_front(&ConnectionRegistry::UpdateTimeout, this));
  auto [position, inserted] =
      connections_.try_emplace(fd, std::move(connection));
  if (!inserted) throw std::logic_error("duplicate connection fd");
  try {
    position->second->Start();
    position->second->last_progress_ = timer::TimerQueue::Clock::now();
    UpdateTimeout(*position->second, false);
  } catch (...) {
    position->second->Stop();
    DrainClosedConnections();
    connections_.erase(fd);
    throw;
  }
}

void ConnectionRegistry::CancelTimeout(TcpConnection& connection) noexcept {
  if (connection.timeout_id_) {
    loop_.CancelTimer(connection.timeout_id_);
    connection.timeout_id_ = 0;
  }
}

void ConnectionRegistry::UpdateTimeout(TcpConnection& connection,
                                       bool progress) {
  if (draining_ || connection.state() == TcpConnection::State::kClosing) {
    CancelTimeout(connection);
    return;
  }
  const auto now = timer::TimerQueue::Clock::now();
  if (progress) connection.last_progress_ = now;
  if (!connection.idle_waiting_)
    connection.wait_since_.reset();
  else if (!connection.wait_since_)
    connection.wait_since_ = now;
  std::optional<timer::TimerQueue::TimePoint> deadline;
  if (timeouts_.idle.count())
    deadline = connection.last_progress_ + timeouts_.idle;
  if (timeouts_.keep_alive.count() && connection.wait_since_) {
    const auto keep_deadline = *connection.wait_since_ + timeouts_.keep_alive;
    if (!deadline || keep_deadline < *deadline) deadline = keep_deadline;
  }
  if (!deadline) {
    CancelTimeout(connection);
    return;
  }
  try {
    if (connection.timeout_id_) {
      if (!loop_.RescheduleTimer(connection.timeout_id_, *deadline))
        connection.RequestClose();
    } else {
      connection.timeout_id_ =
          loop_.AddTimer(*deadline,
                         std::bind_front(&ConnectionRegistry::ExpireConnection,
                                         this,
                                         connection.fd(),
                                         connection.identity()));
    }
  } catch (...) {
    connection.RequestClose();
    throw;
  }
}

void ConnectionRegistry::ExpireConnection(int fd,
                                          TcpConnection::Identity identity) {
  if (draining_) return;
  const auto found = connections_.find(fd);
  if (found == connections_.end() || found->second->identity() != identity)
    return;
  found->second->timeout_id_ = 0;
  found->second->RequestClose();
}

void ConnectionRegistry::OnConnectionClosed(
    int fd,
    TcpConnection::Identity identity) noexcept {
  const auto found = connections_.find(fd);
  if (found == connections_.end() || found->second->identity() != identity)
    return;
  auto& connection = *found->second;
  if (connection.queued_for_recovery_) return;
  connection.queued_for_recovery_ = true;
  connection.next_closing_ = closing_head_;
  closing_head_ = &connection;
}

void ConnectionRegistry::DrainClosedConnections() noexcept {
  while (closing_head_) {
    auto* connection = closing_head_;
    closing_head_ = connection->next_closing_;
    const auto found = connections_.find(connection->fd());
    if (found != connections_.end() &&
        found->second->identity() == connection->identity())
      connections_.erase(found);
  }
  if (draining_ && connections_.empty() && !notified_) {
    notified_ = true;
    if (request_stop_callback_) request_stop_callback_();
  }
}
}  // namespace hp::net
