#include "net/socket.h"

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <system_error>
#include <utility>

namespace hp::net {
namespace {

[[noreturn]] void ThrowSystemError(std::string operation) {
  const int error_number = errno;
  throw std::system_error(error_number,
                          std::generic_category(),
                          std::move(operation));
}

}  // namespace

Socket::Socket(int fd) noexcept : fd_(fd) {}

Socket::~Socket() noexcept { Reset(); }

Socket::Socket(Socket&& other) noexcept : fd_(other.Release()) {}

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    Reset(other.Release());
  }
  return *this;
}

Socket Socket::CreateTcp() {
  const int fd =
      ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd == -1) {
    ThrowSystemError("socket(AF_INET, SOCK_STREAM)");
  }
  return Socket(fd);
}

int Socket::fd() const noexcept { return fd_; }

bool Socket::valid() const noexcept { return fd_ >= 0; }

int Socket::Release() noexcept { return std::exchange(fd_, kInvalidFd); }

void Socket::Reset(int new_fd) noexcept {
  if (fd_ == new_fd) {
    return;
  }

  const int old_fd = std::exchange(fd_, new_fd);
  if (old_fd >= 0) {
    ::close(old_fd);
  }
}

void Socket::set_non_blocking() {
  const int flags = ::fcntl(fd_, F_GETFL);
  if (flags == -1) {
    ThrowSystemError("fcntl(F_GETFL)");
  }
  if (::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) == -1) {
    ThrowSystemError("fcntl(F_SETFL)");
  }
}

void Socket::set_reuse_address(bool enabled) {
  const int option = enabled ? 1 : 0;
  if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option)) ==
      -1) {
    ThrowSystemError("setsockopt(SO_REUSEADDR)");
  }
}

void Socket::BindAny(std::uint16_t port) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(port);

  if (::bind(fd_,
             reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) == -1) {
    ThrowSystemError("bind");
  }
}

void Socket::Listen(int backlog) {
  if (::listen(fd_, backlog) == -1) {
    ThrowSystemError("listen");
  }
}

Socket Socket::AcceptNonBlocking() {
  while (true) {
    const int accepted_fd =
        ::accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (accepted_fd >= 0) {
      return Socket(accepted_fd);
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return Socket();
    }
    ThrowSystemError("accept4");
  }
}

std::uint16_t Socket::local_port() const {
  sockaddr_in address{};
  socklen_t address_length = sizeof(address);
  if (::getsockname(fd_,
                    reinterpret_cast<sockaddr*>(&address),
                    &address_length) == -1) {
    ThrowSystemError("getsockname");
  }
  if (address.sin_family != AF_INET || address_length < sizeof(address)) {
    throw std::system_error(EAFNOSUPPORT,
                            std::generic_category(),
                            "getsockname(AF_INET)");
  }
  return ntohs(address.sin_port);
}

int Socket::socket_error() const {
  int error_number = 0;
  socklen_t error_length = sizeof(error_number);
  if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error_number, &error_length) ==
      -1) {
    ThrowSystemError("getsockopt(SO_ERROR)");
  }
  return error_number;
}

}  // namespace hp::net
