#include "net/connection_io.h"

#include <pthread.h>
#include <signal.h>
#include <sys/sendfile.h>
#include <sys/socket.h>

#include <algorithm>
#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace hp::net {
namespace {
struct FileWrite {
  ssize_t count;
  int error;
};

FileWrite send_file(int socket, int file, off_t* offset,
                    std::size_t length) noexcept {
  sigset_t pipe, previous, pending;
  ::sigemptyset(&pipe);
  ::sigaddset(&pipe, SIGPIPE);
  const int blocked = ::pthread_sigmask(SIG_BLOCK, &pipe, &previous);
  if (blocked) return {-1, blocked};
  if (::sigpending(&pending) < 0) {
    const int error = errno;
    ::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    return {-1, error};
  }
  const auto count = ::sendfile(socket, file, offset, length);
  int error = count < 0 ? errno : 0;
  // Preserve an already blocked or pending SIGPIPE. Only consume this call's
  // newly generated signal when restoring an originally unblocked caller.
  if (error == EPIPE && ::sigismember(&previous, SIGPIPE) == 0 &&
      ::sigismember(&pending, SIGPIPE) == 0) {
    const timespec immediate{};
    int consumed;
    do {
      consumed = ::sigtimedwait(&pipe, nullptr, &immediate);
    } while (consumed < 0 && errno == EINTR);
  }
  const int restored = ::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
  if (restored && !error) error = restored;
  return {count, error};
}
}  // namespace

ConnectionIo::ConnectionIo(Socket socket, std::size_t max_input_bytes) noexcept
    : socket_(std::move(socket)),
      max_input_bytes_(max_input_bytes),
      input_(max_input_bytes ? max_input_bytes
                             : std::numeric_limits<std::size_t>::max()) {}

int ConnectionIo::fd() const noexcept { return socket_.fd(); }

int ConnectionIo::socket_error() const { return socket_.socket_error(); }

std::span<const std::byte> ConnectionIo::input_view() const noexcept {
  return input_.readable_view();
}

void ConnectionIo::consume(std::size_t count) { input_.consume(count); }

ReadResult ConnectionIo::read_once() {
  ReadResult result;
  if (peer_half_closed_) {
    result.peer_closed = true;
    return result;
  }
  std::size_t budget = 16U * 1024U;
  if (max_input_bytes_) {
    if (input_.readable_bytes() >= max_input_bytes_) {
      result.error_number = EMSGSIZE;
      return result;
    }
    budget = std::min(budget, max_input_bytes_ - input_.readable_bytes());
  }
  const auto available = input_.writable_bytes();
  const auto size = std::min(budget, available ? available : std::size_t{4096});
  auto tail = input_.prepare(size);
  while (true) {
    const auto count = ::recv(fd(), tail.data(), tail.size(), 0);
    if (count > 0) {
      input_.commit(static_cast<std::size_t>(count));
      result.bytes_read = static_cast<std::size_t>(count);
      return result;
    }
    if (count == 0) {
      peer_half_closed_ = true;
      result.peer_closed = true;
      return result;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      result.would_block = true;
    else
      result.error_number = errno;
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
  result.file_transfer = file_.has_value();
  std::size_t calls = 0;
  while (output_.readable_bytes()) {
    if (result.file_transfer && calls++ == file_call_budget) return result;
    const auto* data = output_.readable_view().data();
    const std::size_t remaining = output_.readable_bytes();
    const ssize_t count = ::send(socket_.fd(), data, remaining, MSG_NOSIGNAL);
    if (count > 0) {
      const auto byte_count = static_cast<std::size_t>(count);
      output_.consume(byte_count);
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

  std::size_t progress = 0;
  while (file_ && file_->remaining()) {
    if (calls++ >= file_call_budget || progress == file_write_budget)
      return result;
    off_t offset = file_->offset();
    const auto count =
        std::min(file_->remaining(), file_write_budget - progress);
    const auto written = send_file(fd(), file_->fd(), &offset, count);
    if (written.count > 0) {
      const auto bytes = static_cast<std::size_t>(written.count);
      file_->advance(bytes);
      progress += bytes;
      result.bytes_written += bytes;
    }
    if (written.error == EINTR) continue;
    if (written.error == EAGAIN || written.error == EWOULDBLOCK) {
      result.would_block = true;
      return result;
    }
    if (written.error || written.count == 0) {
      result.error_number = written.error ? written.error : EIO;
      return result;
    }
  }
  file_.reset();
  output_.release_empty(64U * 1024U);
  return result;
}

bool ConnectionIo::output_fits(std::size_t pending,
                               std::size_t incoming) noexcept {
  return pending <= output_limit && incoming <= output_limit - pending;
}

void ConnectionIo::queue_output(std::span<const std::byte> bytes) {
  if (file_) throw std::logic_error("append while file output is pending");
  if (!output_fits(pending_bytes(), bytes.size()))
    throw std::length_error("connection output limit exceeded");
  output_.append(bytes);
}

void ConnectionIo::queue_file(std::span<const std::byte> header,
                              base::FileRegion file) {
  if (file.fd() < 0)
    throw std::invalid_argument("submission of moved file region");
  if (has_pending_output() || file_)
    throw std::logic_error("file submission while output is pending");
  if (!output_fits(header.size(), file.remaining()))
    throw std::length_error("connection output limit exceeded");
  // Allocate before taking the region; failure destroys the by-value owner.
  output_.append(header);
  file_.emplace(std::move(file));
  if (!file_->remaining()) file_.reset();
}

void ConnectionIo::mark_peer_half_closed() noexcept {
  peer_half_closed_ = true;
}

bool ConnectionIo::peer_half_closed() const noexcept {
  return peer_half_closed_;
}

bool ConnectionIo::has_pending_output() const noexcept {
  return output_.readable_bytes() != 0 || (file_ && file_->remaining());
}

bool ConnectionIo::accepts_input() const noexcept { return !peer_half_closed_; }

std::size_t ConnectionIo::pending_bytes() const noexcept {
  return output_.readable_bytes() + (file_ ? file_->remaining() : 0);
}

bool ConnectionIo::ready_to_close() const noexcept {
  return peer_half_closed_ && !has_pending_output();
}

}  // namespace hp::net
