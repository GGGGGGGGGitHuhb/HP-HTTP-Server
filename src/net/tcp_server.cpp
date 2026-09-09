#include "net/tcp_server.h"
#include <limits>
#include <stdexcept>
#include <utility>

namespace hp::net {
TcpServer::TcpServer(std::uint16_t requested_port, MessageCallbackFactory factory,
                     std::size_t max_input_bytes)
    : callback_factory_(std::move(factory)), max_input_bytes_(max_input_bytes),
      acceptor_(loop_, requested_port, [this](Socket socket) {
          add_connection(std::move(socket));
      }) {
    loop_.set_after_dispatch([this] {
        drain_closed_connections();
    });
    acceptor_.start();
}

TcpServer::~TcpServer() noexcept {
    acceptor_.stop();
    for (auto& [fd, connection] : connections_) {
        (void)fd;
        connection->stop();
    }
    connections_.clear();
}

std::uint16_t TcpServer::bound_port() const noexcept {
    return acceptor_.bound_port();
}

void TcpServer::run() {
    loop_.loop();
}

void TcpServer::add_connection(Socket socket) {
    if (next_identity_ == 0)
        throw std::overflow_error("connection identity exhausted");
    const auto identity = next_identity_;
    next_identity_ =
        identity == std::numeric_limits<TcpConnection::Identity>::max() ? 0 : identity + 1;
    const int fd = socket.fd();
    auto connection = std::make_unique<TcpConnection>(
        loop_, std::move(socket), identity,
        callback_factory_ ? callback_factory_() : TcpConnection::MessageCallback{},
        max_input_bytes_, [this](int closed_fd, TcpConnection::Identity id) noexcept {
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

void TcpServer::connection_closed(int fd, TcpConnection::Identity identity) noexcept {
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

void TcpServer::drain_closed_connections() noexcept {
    while (closing_head_) {
        auto* connection = closing_head_;
        closing_head_ = connection->next_closing_;
        const auto found = connections_.find(connection->fd());
        if (found != connections_.end() && found->second->identity() == connection->identity())
            connections_.erase(found);
    }
}
}
