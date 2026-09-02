#include "net/epoller.h"

#include <cerrno>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>

namespace hp::net {
namespace {

[[noreturn]] void throw_system_error(const char* operation) {
    const int error_number = errno;
    throw std::system_error(error_number, std::generic_category(), operation);
}

}  // namespace

Epoller::Epoller(std::size_t initial_capacity)
    : fd_(::epoll_create1(EPOLL_CLOEXEC)), events_(initial_capacity) {
    if (fd_ == -1) {
        throw_system_error("epoll_create1");
    }
    if (events_.empty()) {
        events_.resize(1);
    }
}

Epoller::~Epoller() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

int Epoller::fd() const noexcept {
    return fd_;
}

void Epoller::control(int operation, int observed_fd, std::uint32_t events,
                      std::uint64_t token) {
    epoll_event event{};
    event.events = events;
    event.data.u64 = token;
    if (::epoll_ctl(fd_, operation, observed_fd, &event) == -1) {
        throw_system_error("epoll_ctl");
    }
}

void Epoller::add(int observed_fd, std::uint32_t events,
                  std::uint64_t token) {
    control(EPOLL_CTL_ADD, observed_fd, events, token);
}

void Epoller::modify(int observed_fd, std::uint32_t events,
                     std::uint64_t token) {
    control(EPOLL_CTL_MOD, observed_fd, events, token);
}

void Epoller::remove(int observed_fd) noexcept {
    if (observed_fd < 0) {
        return;
    }
    if (::epoll_ctl(fd_, EPOLL_CTL_DEL, observed_fd, nullptr) == -1 &&
        errno != ENOENT && errno != EBADF) {
        // Closing the sole Socket owner still removes the fd from epoll.
    }
}

std::span<const epoll_event> Epoller::wait(int timeout_ms) {
    while (true) {
        const int count = ::epoll_wait(fd_, events_.data(),
                                       static_cast<int>(events_.size()),
                                       timeout_ms);
        if (count >= 0) {
            ready_count_ = static_cast<std::size_t>(count);
            return {events_.data(), ready_count_};
        }
        if (errno == EINTR) {
            continue;
        }
        throw_system_error("epoll_wait");
    }
}

}  // namespace hp::net
