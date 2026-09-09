#include "net/connection_registry.h"
#include <limits>
#include <stdexcept>
#include <utility>

namespace hp::net {
ConnectionRegistry::ConnectionRegistry(EventLoop& loop, std::size_t max_input_bytes)
    : loop_(loop), max_input_bytes_(max_input_bytes) {
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

void ConnectionRegistry::add(Socket socket, TcpConnection::MessageCallback callback) {
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
    auto [position, inserted] = connections_.try_emplace(fd, std::move(connection));
    if (!inserted)
        throw std::logic_error("duplicate connection fd");
    try {
        position->second->start();
    } catch (...) {
        connections_.erase(position);
        throw;
    }
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
}
} // namespace hp::net
