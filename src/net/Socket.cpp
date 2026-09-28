#include "net/Socket.h"

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <system_error>
#include <utility>

namespace hp::net {
namespace {

[[noreturn]] void throwSystemError(std::string operation) {
  const int errorNumber = errno;
  throw std::system_error(errorNumber, std::generic_category(), std::move(operation));
}

}  // namespace

Socket::Socket(int fd) noexcept : fd_(fd) {}

Socket::~Socket() noexcept { reset(); }

Socket::Socket(Socket&& other) noexcept : fd_(other.release()) {}

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    reset(other.release());
  }
  return *this;
}

Socket Socket::createTcp() {
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd == -1) {
    throwSystemError("socket(AF_INET, SOCK_STREAM)");
  }
  return Socket(fd);
}

int Socket::fd() const noexcept { return fd_; }

bool Socket::valid() const noexcept { return fd_ >= 0; }

int Socket::release() noexcept { return std::exchange(fd_, kInvalidFd); }

void Socket::reset(int newFd) noexcept {
  if (fd_ == newFd) {
    return;
  }

  const int oldFd = std::exchange(fd_, newFd);
  if (oldFd >= 0) {
    ::close(oldFd);
  }
}

void Socket::setNonBlocking() {
  const int flags = ::fcntl(fd_, F_GETFL);
  if (flags == -1) {
    throwSystemError("fcntl(F_GETFL)");
  }
  if (::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) == -1) {
    throwSystemError("fcntl(F_SETFL)");
  }
}

void Socket::setReuseAddress(bool enabled) {
  const int option = enabled ? 1 : 0;
  if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option)) == -1) {
    throwSystemError("setsockopt(SO_REUSEADDR)");
  }
}

void Socket::setTcpNoDelay(bool enabled) {
  const int option = enabled ? 1 : 0;
  if (::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &option, sizeof(option)) == -1) {
    throwSystemError("setsockopt(TCP_NODELAY)");
  }
}

void Socket::bindAny(std::uint16_t port) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(port);

  if (::bind(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == -1) {
    throwSystemError("bind");
  }
}

void Socket::listen(int backlog) {
  if (::listen(fd_, backlog) == -1) {
    throwSystemError("listen");
  }
}

Socket Socket::acceptNonBlocking() {
  while (true) {
    const int acceptedFd = ::accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (acceptedFd >= 0) {
      return Socket(acceptedFd);
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return Socket();
    }
    throwSystemError("accept4");
  }
}

std::uint16_t Socket::localPort() const {
  sockaddr_in address{};
  socklen_t addressLength = sizeof(address);
  if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &addressLength) == -1) {
    throwSystemError("getsockname");
  }
  if (address.sin_family != AF_INET || addressLength < sizeof(address)) {
    throw std::system_error(EAFNOSUPPORT, std::generic_category(), "getsockname(AF_INET)");
  }
  return ntohs(address.sin_port);
}

int Socket::socketError() const {
  int errorNumber = 0;
  socklen_t errorLength = sizeof(errorNumber);
  if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &errorNumber, &errorLength) == -1) {
    throwSystemError("getsockopt(SO_ERROR)");
  }
  return errorNumber;
}

}  // namespace hp::net
