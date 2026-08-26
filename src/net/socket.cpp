#include "net/socket.h"

#include <cerrno>
#include <fcntl.h>
#include <string>
#include <system_error>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace hp::net {
namespace {

[[noreturn]] void throw_system_error(std::string operation) {
    const int error_number = errno;
    throw std::system_error(error_number, std::generic_category(),
                            std::move(operation));
}

}  // namespace

Socket::Socket(int fd) noexcept : fd_(fd) {}

Socket::~Socket() noexcept {
    reset();
}

Socket::Socket(Socket&& other) noexcept : fd_(other.release()) {}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

int Socket::fd() const noexcept {
    return fd_;
}

bool Socket::valid() const noexcept {
    return fd_ >= 0;
}

int Socket::release() noexcept {
    return std::exchange(fd_, invalid_fd);
}

void Socket::reset(int new_fd) noexcept {
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
        throw_system_error("fcntl(F_GETFL)");
    }
    if (::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) == -1) {
        throw_system_error("fcntl(F_SETFL)");
    }
}

void Socket::set_reuse_address(bool enabled) {
    const int option = enabled ? 1 : 0;
    if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &option,
                     sizeof(option)) == -1) {
        throw_system_error("setsockopt(SO_REUSEADDR)");
    }
}

}  // namespace hp::net
