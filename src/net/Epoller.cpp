#include "net/Epoller.h"

#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <string>
#include <system_error>

namespace hp::net {
namespace {

[[noreturn]] void throwSystemError(const char* operation) {
  const int errorNumber = errno;
  throw std::system_error(errorNumber, std::generic_category(), operation);
}

}  // namespace

Epoller::Epoller(std::size_t initialCapacity)
    : events_(initialCapacity == 0 ? 1 : initialCapacity) {
  fd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (fd_ == -1) {
    throwSystemError("epoll_create1");
  }
}

Epoller::~Epoller() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

int Epoller::fd() const noexcept { return fd_; }

void Epoller::controlDescriptor(int operation,
                                int observedFd,
                                std::uint32_t events,
                                std::uint64_t token) {
  epoll_event event{};
  event.events = events;
  event.data.u64 = token;
  if (::epoll_ctl(fd_, operation, observedFd, &event) == -1) {
    throwSystemError("epoll_ctl");
  }
}

void Epoller::addDescriptor(int observedFd, std::uint32_t events, std::uint64_t token) {
  controlDescriptor(EPOLL_CTL_ADD, observedFd, events, token);
}

void Epoller::modifyDescriptor(int observedFd,
                     std::uint32_t events,
                     std::uint64_t token) {
  controlDescriptor(EPOLL_CTL_MOD, observedFd, events, token);
}

void Epoller::removeDescriptor(int observedFd) noexcept {
  if (observedFd < 0) {
    return;
  }
  if (::epoll_ctl(fd_, EPOLL_CTL_DEL, observedFd, nullptr) == -1 &&
      errno != ENOENT && errno != EBADF) {
    // 关闭唯一持有所有权的 Socket 仍会将 fd 从 epoll 中移除。
  }
}

std::span<const epoll_event> Epoller::waitForEvents(int timeoutMs) {
  while (true) {
    const int count = ::epoll_wait(fd_,
                                   events_.data(),
                                   static_cast<int>(events_.size()),
                                   timeoutMs);
    if (count >= 0) {
      readyCount_ = static_cast<std::size_t>(count);
      return {events_.data(), readyCount_};
    }
    if (errno == EINTR) {
      continue;
    }
    throwSystemError("epoll_wait");
  }
}

}  // namespace hp::net
