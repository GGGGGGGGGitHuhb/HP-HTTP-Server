#include "net/connection_io.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <stdexcept>
#include <sys/socket.h>
#include <system_error>
#include <utility>

namespace hp::net {
ConnectionIo::ConnectionIo(Socket socket, std::size_t max_input_bytes) noexcept
    : socket_(std::move(socket)), max_input_bytes_(max_input_bytes) {}
int ConnectionIo::fd() const noexcept { return socket_.fd(); }
int ConnectionIo::socket_error() const { return socket_.socket_error(); }
std::span<const std::byte> ConnectionIo::input_view() const noexcept { return input_; }
void ConnectionIo::consume(std::size_t count) {
    if (count > input_.size()) throw std::out_of_range("input consumption exceeds buffered bytes");
    input_.erase(input_.begin(), input_.begin() + static_cast<std::ptrdiff_t>(count));
}
ReadResult ConnectionIo::read_once() {
    ReadResult result;
    if (peer_half_closed_) { result.peer_closed = true; return result; }
    std::array<std::byte, 16 * 1024> buffer{};
    std::size_t size = buffer.size();
    if (max_input_bytes_) {
        if (input_.size() >= max_input_bytes_) { result.error_number = EMSGSIZE; return result; }
        size = std::min(size, max_input_bytes_ - input_.size());
    }
    while (true) {
        const auto count = ::recv(fd(), buffer.data(), size, 0);
        if (count > 0) {
            input_.insert(input_.end(), buffer.begin(), buffer.begin() + count);
            result.bytes_read = static_cast<std::size_t>(count);
            return result;
        }
        if (count == 0) { peer_half_closed_ = true; result.peer_closed = true; return result; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) result.would_block = true;
        else result.error_number = errno;
        return result;
    }
}
ReadResult ConnectionIo::read_available() {
    ReadResult total;
    while (true) {
        auto read = read_once();
        total.bytes_read += read.bytes_read;
        if (read.bytes_read) continue;
        total.would_block = read.would_block;
        total.peer_closed = read.peer_closed;
        total.error_number = read.error_number;
        return total;
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
    return !peer_half_closed_;
}

std::size_t ConnectionIo::pending_bytes() const noexcept {
    return output_.size() - write_offset_;
}

bool ConnectionIo::ready_to_close() const noexcept {
    return peer_half_closed_ && !has_pending_output();
}

}  // namespace hp::net
