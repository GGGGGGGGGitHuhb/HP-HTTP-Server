#include "net/tcp_server.h"

#include "base/logger.h"

#include <array>
#include <cerrno>
#include <string>
#include <system_error>
#include <sys/socket.h>
#include <utility>

namespace hp::net {
namespace {

std::string connection_error_message(int fd, const char* operation,
                                     int error_number) {
    return "connection fd " + std::to_string(fd) + " " + operation +
           " failed: " + std::generic_category().message(error_number) +
           " (" + std::to_string(error_number) + ")";
}

}  // namespace

ConnectionIo::ConnectionIo(Socket socket) noexcept : socket_(std::move(socket)) {}

int ConnectionIo::fd() const noexcept {
    return socket_.fd();
}

ReadResult ConnectionIo::read_available() {
    ReadResult result;
    std::array<std::byte, 16 * 1024> buffer{};

    while (true) {
        const ssize_t count =
            ::recv(socket_.fd(), buffer.data(), buffer.size(), 0);
        if (count > 0) {
            const auto byte_count = static_cast<std::size_t>(count);
            queue_output({buffer.data(), byte_count});
            result.bytes_read += byte_count;
            continue;
        }
        if (count == 0) {
            peer_half_closed_ = true;
            result.peer_closed = true;
            return result;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            result.would_block = true;
            return result;
        }
        result.error_number = errno;
        return result;
    }
}

WriteResult ConnectionIo::write_available() {
    WriteResult result;
    while (write_offset_ < output_.size()) {
        const auto* data = output_.data() + write_offset_;
        const std::size_t remaining = output_.size() - write_offset_;
        const ssize_t count =
            ::send(socket_.fd(), data, remaining, MSG_NOSIGNAL);
        if (count > 0) {
            const auto byte_count = static_cast<std::size_t>(count);
            write_offset_ += byte_count;
            result.bytes_written += byte_count;
            continue;
        }
        if (count == 0) {
            result.error_number = EPIPE;
            return result;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            result.would_block = true;
            return result;
        }
        result.error_number = errno;
        return result;
    }

    output_.clear();
    write_offset_ = 0;
    return result;
}

ConnectionEventResult ConnectionIo::handle_event(std::uint32_t events) {
    ConnectionEventResult result;

    if ((events & EPOLLERR) != 0U) {
        try {
            result.socket_error = socket_.socket_error();
            result.socket_error_observed = true;
        } catch (const std::system_error& error) {
            result.socket_error_query_error = error.code().value();
        }
        result.close_requested = true;
    }

    if ((events & EPOLLIN) != 0U) {
        const ReadResult read = read_available();
        result.bytes_read = read.bytes_read;
        result.read_error = read.error_number;
        if (read.error_number != 0) {
            result.close_requested = true;
        }
    }

    if ((events & EPOLLRDHUP) != 0U) {
        mark_peer_half_closed();
    }

    if (has_pending_output()) {
        const WriteResult write = write_available();
        result.bytes_written = write.bytes_written;
        result.write_error = write.error_number;
        if (write.error_number != 0) {
            result.close_requested = true;
        }
    }

    if ((events & EPOLLHUP) != 0U || ready_to_close()) {
        result.close_requested = true;
    }
    return result;
}

void ConnectionIo::queue_output(std::span<const std::byte> bytes) {
    output_.insert(output_.end(), bytes.begin(), bytes.end());
}

void ConnectionIo::mark_peer_half_closed() noexcept {
    peer_half_closed_ = true;
}

bool ConnectionIo::peer_half_closed() const noexcept {
    return peer_half_closed_;
}

bool ConnectionIo::has_pending_output() const noexcept {
    return write_offset_ < output_.size();
}

std::size_t ConnectionIo::pending_bytes() const noexcept {
    return output_.size() - write_offset_;
}

bool ConnectionIo::ready_to_close() const noexcept {
    return peer_half_closed_ && !has_pending_output();
}

Socket TcpServer::create_listener(std::uint16_t port) {
    Socket listener = Socket::create_tcp();
    listener.set_reuse_address(true);
    listener.bind_any(port);
    listener.listen(128);
    return listener;
}

TcpServer::TcpServer(std::uint16_t requested_port)
    : listener_(create_listener(requested_port)),
      epoller_(),
      bound_port_(listener_.local_port()) {
    epoller_.add(listener_.fd(), listener_events, listener_token);
}

std::uint16_t TcpServer::bound_port() const noexcept {
    return bound_port_;
}

std::uint64_t TcpServer::make_token(int fd, std::uint32_t generation) {
    return (static_cast<std::uint64_t>(generation) << 32U) |
           static_cast<std::uint32_t>(fd);
}

int TcpServer::token_fd(std::uint64_t token) {
    return static_cast<int>(static_cast<std::uint32_t>(token));
}

std::uint32_t TcpServer::token_generation(std::uint64_t token) {
    return static_cast<std::uint32_t>(token >> 32U);
}

std::uint32_t TcpServer::next_generation() noexcept {
    const std::uint32_t generation = next_generation_++;
    if (next_generation_ == 0) {
        next_generation_ = 1;
    }
    return generation == 0 ? next_generation_++ : generation;
}

[[noreturn]] void TcpServer::run() {
    while (true) {
        const auto events = epoller_.wait(-1);
        for (const epoll_event& event : events) {
            if (event.data.u64 == listener_token) {
                handle_listener_event(event.events);
            } else {
                handle_connection_event(event.data.u64, event.events);
            }
        }
    }
}

void TcpServer::handle_listener_event(std::uint32_t events) {
    if ((events & EPOLLIN) != 0U) {
        accept_ready_connections();
    }
    if ((events & (EPOLLERR | EPOLLHUP)) != 0U) {
        const int error_number = listener_.socket_error();
        throw std::system_error(error_number == 0 ? EIO : error_number,
                                std::generic_category(),
                                "listener epoll event");
    }
}

void TcpServer::accept_ready_connections() {
    while (true) {
        Socket accepted;
        try {
            accepted = listener_.accept_non_blocking();
        } catch (const std::system_error& error) {
            base::warn(std::string("accept4 failed: ") + error.what());
            return;
        }
        if (!accepted.valid()) {
            return;
        }

        const int accepted_fd = accepted.fd();
        const std::uint32_t generation = next_generation();
        const std::uint64_t token = make_token(accepted_fd, generation);
        try {
            epoller_.add(accepted_fd, connection_read_events, token);
        } catch (const std::system_error& error) {
            base::warn(std::string("connection epoll registration failed: ") +
                       error.what());
            continue;
        }

        try {
            connections_.emplace(
                accepted_fd,
                ConnectionState{ConnectionIo(std::move(accepted)), generation});
        } catch (...) {
            epoller_.remove(accepted_fd);
            throw;
        }
    }
}

void TcpServer::handle_connection_event(std::uint64_t token,
                                        std::uint32_t events) {
    const int fd = token_fd(token);
    const auto found = connections_.find(fd);
    if (found == connections_.end() ||
        found->second.generation != token_generation(token)) {
        return;
    }

    ConnectionState& connection = found->second;
    const ConnectionEventResult result = connection.io.handle_event(events);
    if (result.socket_error_observed && result.socket_error != 0) {
        base::warn(connection_error_message(fd, "SO_ERROR",
                                            result.socket_error));
    }
    if (result.socket_error_query_error != 0) {
        base::warn(connection_error_message(fd, "getsockopt(SO_ERROR)",
                                            result.socket_error_query_error));
    }
    if (result.read_error != 0) {
        base::warn(connection_error_message(fd, "recv", result.read_error));
    }
    if (result.write_error != 0) {
        base::warn(connection_error_message(fd, "send", result.write_error));
    }

    if (result.close_requested) {
        close_connection(fd);
        return;
    }

    try {
        update_interest(connection);
    } catch (const std::system_error& error) {
        base::warn(std::string("connection epoll update failed: ") +
                   error.what());
        close_connection(fd);
    }
}

void TcpServer::update_interest(ConnectionState& connection) {
    std::uint32_t events = connection.io.peer_half_closed()
                               ? 0U
                               : connection_read_events;
    if (connection.io.has_pending_output()) {
        events |= EPOLLOUT;
    }
    epoller_.modify(connection.io.fd(), events,
                    make_token(connection.io.fd(), connection.generation));
}

void TcpServer::close_connection(int fd) noexcept {
    const auto found = connections_.find(fd);
    if (found == connections_.end()) {
        return;
    }
    epoller_.remove(fd);
    connections_.erase(found);
}

}  // namespace hp::net
