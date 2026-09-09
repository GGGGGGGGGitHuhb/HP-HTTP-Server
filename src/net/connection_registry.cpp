#include "net/connection_registry.h"
#include <limits>
#include <stdexcept>
#include <utility>

namespace hp::net {
ConnectionRegistry::ConnectionRegistry(EventLoop& loop, std::size_t max_input_bytes,
                                       ConnectionTimeouts timeouts)
    : timeouts_(timeouts), loop_(loop), max_input_bytes_(max_input_bytes) {
    loop_.set_after_dispatch([this] {
        drain_closed_connections();
    });
}

ConnectionRegistry::~ConnectionRegistry() noexcept {
    for (auto& [fd, connection] : connections_) {
        (void)fd;
        connection->stop();
    }
    connections_.clear();
    loop_.set_after_dispatch({});
}

void ConnectionRegistry::set_drained_callback(EventLoop::Task callback) {
    drained_callback_ = std::move(callback);
}

void ConnectionRegistry::begin_drain(bool force) {
    draining_ = true;
    for (auto& [fd, connection] : connections_) {
        (void)fd;
        cancel_timeout(*connection);
        if (force)
            connection->request_close();
        else
            connection->begin_drain();
    }
    drain_closed_connections();
}

void ConnectionRegistry::add(Socket socket, TcpConnection::MessageCallback callback) {
    if (draining_)
        return;
    if (next_identity_ == 0)
        throw std::overflow_error("connection identity exhausted");
    const auto identity = next_identity_;
    next_identity_ =
        identity == std::numeric_limits<TcpConnection::Identity>::max() ? 0 : identity + 1;
    const int fd = socket.fd();
    auto connection = std::make_unique<TcpConnection>(
        loop_, std::move(socket), identity, std::move(callback), max_input_bytes_,
        [this](int closed_fd, TcpConnection::Identity id) noexcept {
            connection_closed(closed_fd, id);
        });
    connection->activity_callback_ = [this](TcpConnection& c, bool progress) {
        update_timeout(c, progress);
    };
    auto [position, inserted] = connections_.try_emplace(fd, std::move(connection));
    if (!inserted)
        throw std::logic_error("duplicate connection fd");
    try {
        position->second->start();
        position->second->last_progress_ = timer::TimerQueue::Clock::now();
        update_timeout(*position->second, false);
    } catch (...) {
        position->second->stop();
        drain_closed_connections();
        connections_.erase(fd);
        throw;
    }
}

void ConnectionRegistry::cancel_timeout(TcpConnection& connection) noexcept {
    if (connection.timeout_id_) {
        loop_.cancel_timer(connection.timeout_id_);
        connection.timeout_id_ = 0;
    }
}

void ConnectionRegistry::update_timeout(TcpConnection& connection, bool progress) {
    if (draining_ || connection.state() == TcpConnection::State::closing) {
        cancel_timeout(connection);
        return;
    }
    const auto now = timer::TimerQueue::Clock::now();
    if (progress)
        connection.last_progress_ = now;
    if (!connection.idle_waiting_)
        connection.wait_since_.reset();
    else if (!connection.wait_since_)
        connection.wait_since_ = now;
    std::optional<timer::TimerQueue::TimePoint> deadline;
    if (timeouts_.idle.count())
        deadline = connection.last_progress_ + timeouts_.idle;
    if (timeouts_.keep_alive.count() && connection.wait_since_) {
        const auto keep_deadline = *connection.wait_since_ + timeouts_.keep_alive;
        if (!deadline || keep_deadline < *deadline)
            deadline = keep_deadline;
    }
    if (!deadline) {
        cancel_timeout(connection);
        return;
    }
    try {
        if (connection.timeout_id_) {
            if (!loop_.reschedule_timer(connection.timeout_id_, *deadline))
                connection.request_close();
        } else {
            connection.timeout_id_ = loop_.add_timer(*deadline,
                [this, fd = connection.fd(), identity = connection.identity()] {
                    expire(fd, identity);
                });
        }
    } catch (...) {
        connection.request_close();
        throw;
    }
}

void ConnectionRegistry::expire(int fd, TcpConnection::Identity identity) {
    if (draining_)
        return;
    const auto found = connections_.find(fd);
    if (found == connections_.end() || found->second->identity() != identity)
        return;
    found->second->timeout_id_ = 0;
    found->second->request_close();
}

void ConnectionRegistry::connection_closed(int fd, TcpConnection::Identity identity) noexcept {
    const auto found = connections_.find(fd);
    if (found == connections_.end() || found->second->identity() != identity)
        return;
    auto& connection = *found->second;
    if (connection.queued_for_recovery_)
        return;
    connection.queued_for_recovery_ = true;
    connection.next_closing_ = closing_head_;
    closing_head_ = &connection;
}

void ConnectionRegistry::drain_closed_connections() noexcept {
    while (closing_head_) {
        auto* connection = closing_head_;
        closing_head_ = connection->next_closing_;
        const auto found = connections_.find(connection->fd());
        if (found != connections_.end() && found->second->identity() == connection->identity())
            connections_.erase(found);
    }
    if (draining_ && connections_.empty() && !notified_) {
        notified_ = true;
        if (drained_callback_)
            drained_callback_();
    }
}
} // namespace hp::net
