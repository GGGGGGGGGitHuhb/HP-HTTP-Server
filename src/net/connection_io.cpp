#include "net/connection_io.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <system_error>
#include <utility>

namespace hp::net {
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

}  // namespace hp::net
