#include "net/ConnectionIo.h"

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
struct FileWriteResult {
  ssize_t count;
  int error;
};

FileWriteResult sendFileWithoutSigpipe(int socket,
                                       int file,
                                       off_t* offset,
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
  // 保留已屏蔽或已挂起的 SIGPIPE。仅在恢复原先未屏蔽的调用方时，
  // 消费本次调用新产生的信号。
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

ConnectionIo::ConnectionIo(Socket socket, std::size_t maxInputBytes) noexcept
    : socket_(std::move(socket)),
      maxInputBytes_(maxInputBytes),
      input_(maxInputBytes ? maxInputBytes : std::numeric_limits<std::size_t>::max()) {}

int ConnectionIo::fd() const noexcept { return socket_.fd(); }

int ConnectionIo::socketError() const { return socket_.socketError(); }

std::span<const std::byte> ConnectionIo::inputView() const noexcept {
  return input_.readableView();
}

void ConnectionIo::consumeInputBytes(std::size_t count) { input_.consumeReadableBytes(count); }

ReadResult ConnectionIo::readOnce() {
  ReadResult result{0, false, false, 0};
  if (peerHalfClosed_) {
    result.peerClosed = true;
    return result;
  }
  std::size_t budget = 16U * 1024U;
  if (maxInputBytes_) {
    if (input_.readableBytes() >= maxInputBytes_) {
      result.errorNumber = EMSGSIZE;
      return result;
    }
    budget = std::min(budget, maxInputBytes_ - input_.readableBytes());
  }
  const auto available = input_.writableBytes();
  const auto size = std::min(budget, available ? available : std::size_t{4096});
  auto tail = input_.prepareWritableBytes(size);
  while (true) {
    const auto count = ::recv(fd(), tail.data(), tail.size(), 0);
    if (count > 0) {
      input_.commitWrittenBytes(static_cast<std::size_t>(count));
      result.bytesRead = static_cast<std::size_t>(count);
      return result;
    }
    // recv() 得到 0 说明是 EOF
    if (count == 0) {
      peerHalfClosed_ = true;
      result.peerClosed = true;
      return result;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      result.wouldBlock = true;
    else
      result.errorNumber = errno;
    return result;
  }
}

ReadResult ConnectionIo::readAvailable() {
  ReadResult total{0, false, false, 0};
  while (true) {
    auto read = readOnce();
    total.bytesRead += read.bytesRead;
    if (read.bytesRead) continue;
    total.wouldBlock = read.wouldBlock;
    total.peerClosed = read.peerClosed;
    total.errorNumber = read.errorNumber;
    return total;
  }
}

WriteResult ConnectionIo::writeAvailable() {
  WriteResult result{0, file_.has_value(), false, 0};
  std::size_t calls = 0;
  while (output_.readableBytes()) {
    if (result.fileTransfer && calls++ == kFileCallBudget) return result;
    const auto* data = output_.readableView().data();
    const std::size_t remaining = output_.readableBytes();
    const ssize_t count = ::send(socket_.fd(), data, remaining, MSG_NOSIGNAL);
    if (count > 0) {
      const auto byteCount = static_cast<std::size_t>(count);
      output_.consumeReadableBytes(byteCount);
      result.bytesWritten += byteCount;
      continue;
    }
    if (count == 0) {
      result.errorNumber = EPIPE;
      return result;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      result.wouldBlock = true;
      return result;
    }
    result.errorNumber = errno;
    return result;
  }

  std::size_t progress = 0;
  while (file_ && file_->remaining()) {
    if (calls++ >= kFileCallBudget || progress == kFileWriteBudget) return result;
    off_t offset = file_->offset();
    const auto count = std::min(file_->remaining(), kFileWriteBudget - progress);
    const auto written = sendFileWithoutSigpipe(fd(), file_->fd(), &offset, count);
    if (written.count > 0) {
      const auto bytes = static_cast<std::size_t>(written.count);
      file_->advanceFileOffset(bytes);
      progress += bytes;
      result.bytesWritten += bytes;
    }
    if (written.error == EINTR) continue;
    if (written.error == EAGAIN || written.error == EWOULDBLOCK) {
      result.wouldBlock = true;
      return result;
    }
    if (written.error || written.count == 0) {
      result.errorNumber = written.error ? written.error : EIO;
      return result;
    }
  }
  file_.reset();
  output_.releaseEmpty(64U * 1024U);
  return result;
}

bool ConnectionIo::outputFits(std::size_t pending, std::size_t incoming) noexcept {
  return pending <= kOutputLimit && incoming <= kOutputLimit - pending;
}

void ConnectionIo::queueOutput(std::span<const std::byte> bytes) {
  if (file_) throw std::logic_error("append while file output is pending");
  if (!outputFits(pendingBytes(), bytes.size()))
    throw std::length_error("connection output limit exceeded");
  output_.appendBytes(bytes);
}

void ConnectionIo::queueFile(std::span<const std::byte> header, base::FileRegion file) {
  if (file.fd() < 0) throw std::invalid_argument("submission of moved file region");
  if (hasPendingOutput() || file_)
    throw std::logic_error("file submission while output is pending");
  if (!outputFits(header.size(), file.remaining()))
    throw std::length_error("connection output limit exceeded");
  // 先分配，再接管文件区域；失败时销毁按值传入的所有权对象。
  output_.appendBytes(header);
  file_.emplace(std::move(file));
  if (!file_->remaining()) file_.reset();
}

void ConnectionIo::markPeerHalfClosed() noexcept { peerHalfClosed_ = true; }

bool ConnectionIo::peerHalfClosed() const noexcept { return peerHalfClosed_; }

bool ConnectionIo::hasPendingOutput() const noexcept {
  return output_.readableBytes() != 0 || (file_ && file_->remaining());
}

bool ConnectionIo::acceptsInput() const noexcept { return !peerHalfClosed_; }

std::size_t ConnectionIo::pendingBytes() const noexcept {
  return output_.readableBytes() + (file_ ? file_->remaining() : 0);
}

bool ConnectionIo::readyToClose() const noexcept { return peerHalfClosed_ && !hasPendingOutput(); }

}  // namespace hp::net
