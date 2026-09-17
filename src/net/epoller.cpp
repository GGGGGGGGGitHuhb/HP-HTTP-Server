#include "net/epoller.h"

#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <string>
#include <system_error>

namespace hp::net {
namespace {

[[noreturn]] void ThrowSystemError(const char* operation) {
  const int error_number = errno;
  throw std::system_error(error_number, std::generic_category(), operation);
}

}  // namespace

Epoller::Epoller(std::size_t initial_capacity)
    : events_(initial_capacity == 0 ? 1 : initial_capacity) {
  fd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (fd_ == -1) {
    ThrowSystemError("epoll_create1");
  }
}

Epoller::~Epoller() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

int Epoller::fd() const noexcept { return fd_; }

void Epoller::Control(int operation,
                      int observed_fd,
                      std::uint32_t events,
                      std::uint64_t token) {
  epoll_event event{};
  event.events = events;
  event.data.u64 = token;
  if (::epoll_ctl(fd_, operation, observed_fd, &event) == -1) {
    ThrowSystemError("epoll_ctl");
  }
}

void Epoller::Add(int observed_fd, std::uint32_t events, std::uint64_t token) {
  Control(EPOLL_CTL_ADD, observed_fd, events, token);
}

void Epoller::Modify(int observed_fd,
                     std::uint32_t events,
                     std::uint64_t token) {
  Control(EPOLL_CTL_MOD, observed_fd, events, token);
}

void Epoller::Remove(int observed_fd) noexcept {
  if (observed_fd < 0) {
    return;
  }
  if (::epoll_ctl(fd_, EPOLL_CTL_DEL, observed_fd, nullptr) == -1 &&
      errno != ENOENT && errno != EBADF) {
    // Closing the sole Socket owner still removes the fd from epoll.
  }
}

std::span<const epoll_event> Epoller::Wait(int timeout_ms) {
  while (true) {
    const int count = ::epoll_wait(fd_,
                                   events_.data(),
                                   static_cast<int>(events_.size()),
                                   timeout_ms);
    if (count >= 0) {
      ready_count_ = static_cast<std::size_t>(count);
      return {events_.data(), ready_count_};
    }
    if (errno == EINTR) {
      continue;
    }
    ThrowSystemError("epoll_wait");
  }
}

}  // namespace hp::net
