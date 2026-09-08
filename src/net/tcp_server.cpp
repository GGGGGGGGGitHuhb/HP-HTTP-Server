#include "net/tcp_server.h"

#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <string>
#include <system_error>
#include <utility>

#include "base/logger.h"

namespace hp::net {
namespace {

std::string connection_error_message(int fd, const char* operation,
                                     int error_number) {
    return "connection fd " + std::to_string(fd) + " " + operation +
           " failed: " + std::generic_category().message(error_number) + " (" +
           std::to_string(error_number) + ")";
}

}  // namespace

ApplicationResult ApplicationResult::need_more() { return {}; }

ApplicationResult ApplicationResult::respond(std::vector<std::byte> bytes) {
    return {ApplicationStatus::response, std::move(bytes)};
}

ConnectionIo::ConnectionIo(Socket socket, ApplicationHandler handler,
                           std::size_t max_input_bytes) noexcept
    : socket_(std::move(socket)),
      application_handler_(std::move(handler)),
      max_input_bytes_(max_input_bytes) {}

int ConnectionIo::fd() const noexcept { return socket_.fd(); }

bool ConnectionIo::process_application(bool peer_closed) {
    if (!application_handler_ || response_queued_) {
        return false;
    }
    const ApplicationResult application =
        application_handler_({input_.data(), input_.size()}, peer_closed);
    if (application.status == ApplicationStatus::need_more) {
        return false;
    }
    input_.clear();
    queue_output(application.response);
    response_queued_ = true;
    close_after_write_ = true;
    return true;
}

ReadResult ConnectionIo::read_available() {
    ReadResult result;
    std::array<std::byte, 16 * 1024> buffer{};

    while (!response_queued_) {
        std::size_t read_size = buffer.size();
        if (application_handler_ && max_input_bytes_ != 0) {
            if (input_.size() >= max_input_bytes_) {
                if (!process_application(false)) {
                    result.error_number = EMSGSIZE;
                }
                return result;
            }
            read_size = std::min(read_size, max_input_bytes_ - input_.size());
        }

        const ssize_t count = ::recv(socket_.fd(), buffer.data(), read_size, 0);
        if (count > 0) {
            const auto byte_count = static_cast<std::size_t>(count);
            result.bytes_read += byte_count;
            if (application_handler_) {
                input_.insert(input_.end(), buffer.begin(),
                              buffer.begin() + count);
                if (process_application(false)) {
                    return result;
                }
            } else {
                queue_output({buffer.data(), byte_count});
            }
            continue;
        }
        if (count == 0) {
            peer_half_closed_ = true;
            result.peer_closed = true;
            (void)process_application(true);
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
    return result;
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

    if ((events & (EPOLLIN | EPOLLRDHUP)) != 0U) {
        const ReadResult read = read_available();
        result.bytes_read = read.bytes_read;
        result.read_error = read.error_number;
        if (read.error_number != 0) {
            result.close_requested = true;
        }
    }

    if ((events & EPOLLRDHUP) != 0U) {
        mark_peer_half_closed();
        if (application_handler_ && !response_queued_) {
            (void)process_application(true);
        }
    }

    if (has_pending_output()) {
        const WriteResult write = write_available();
        result.bytes_written = write.bytes_written;
        result.write_would_block = write.would_block;
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

bool ConnectionIo::accepts_input() const noexcept {
    return !peer_half_closed_ && !response_queued_;
}

std::size_t ConnectionIo::pending_bytes() const noexcept {
    return output_.size() - write_offset_;
}

bool ConnectionIo::ready_to_close() const noexcept {
    return (peer_half_closed_ || close_after_write_) && !has_pending_output();
}

Socket TcpServer::create_listener(std::uint16_t port) {
    Socket listener = Socket::create_tcp();
    listener.set_reuse_address(true);
    listener.bind_any(port);
    listener.listen(128);
    return listener;
}

TcpServer::TcpServer(std::uint16_t requested_port, ApplicationHandler handler,
                     std::size_t max_input_bytes)
    : listener_(create_listener(requested_port)),
      loop_(),
      listener_channel_(loop_, listener_.fd(),
                        [this](std::uint32_t mask) { handle_listener_event(mask); }),
      application_handler_(std::move(handler)),
      max_input_bytes_(max_input_bytes),
      bound_port_(listener_.local_port()) {
    loop_.set_after_dispatch([this] { drain_closed_connections(); });
    listener_channel_.set_interest(listener_events);
}

TcpServer::~TcpServer() noexcept {
    listener_channel_.remove();
    for (auto& [fd, connection] : connections_) {
        (void)fd;
        connection.channel.remove();
    }
    // ConnectionState destroys Channel before ConnectionIo (reverse member order).
    connections_.clear();
}

std::uint16_t TcpServer::bound_port() const noexcept { return bound_port_; }

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
    loop_.loop();
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
            auto [position, inserted] = connections_.try_emplace(
                accepted_fd, std::move(accepted), application_handler_,
                max_input_bytes_, generation, loop_,
                [this, token, accepted_fd](std::uint32_t mask) {
                    try {
                        handle_connection_event(token, mask);
                    } catch (...) {
                        // Isolate callback failures; removal is allocation-free.
                        close_connection(accepted_fd);
                    }
                });
            if (!inserted) throw std::logic_error("duplicate connection fd");
            position->second.channel.set_interest(connection_read_events);
        } catch (const std::exception& error) {
            close_connection(accepted_fd);
            base::warn(std::string("connection registration failed: ") + error.what());
        } catch (...) {
            close_connection(accepted_fd);
        }
    }
}

void TcpServer::handle_connection_event(std::uint64_t token,
                                        std::uint32_t events) {
    const int fd = token_fd(token);
    const auto found = connections_.find(fd);
    if (found == connections_.end() ||
        found->second.closing ||
        found->second.generation != token_generation(token)) {
        return;
    }

    ConnectionState& connection = found->second;
    ConnectionEventResult result;
    try {
        result = connection.io.handle_event(events);
    } catch (const std::exception& error) {
        base::warn("connection fd " + std::to_string(fd) +
                   " application processing failed: " + error.what());
        close_connection(fd);
        return;
    } catch (...) {
        base::warn("connection fd " + std::to_string(fd) +
                   " application processing failed");
        close_connection(fd);
        return;
    }

    if (result.socket_error_observed && result.socket_error != 0) {
        base::warn(
            connection_error_message(fd, "SO_ERROR", result.socket_error));
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
    if (result.write_would_block) {
        base::info("S3 evidence: connection write reached EAGAIN with " +
                   std::to_string(connection.io.pending_bytes()) +
                   " response bytes pending.");
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
    std::uint32_t events =
        connection.io.accepts_input() ? connection_read_events : 0U;
    if (connection.io.has_pending_output()) {
        events |= EPOLLOUT;
    }
    connection.channel.set_interest(events);
}

void TcpServer::close_connection(int fd) noexcept {
    const auto found = connections_.find(fd);
    if (found == connections_.end() || found->second.closing) return;
    auto& connection = found->second;
    connection.closing = true;
    connection.channel.remove();
    connection.next_closing = closing_head_;
    closing_head_ = &connection;
}

void TcpServer::drain_closed_connections() noexcept {
    while (closing_head_) {
        auto* connection = closing_head_;
        closing_head_ = connection->next_closing;
        connections_.erase(connection->io.fd());
    }
}

}  // namespace hp::net
