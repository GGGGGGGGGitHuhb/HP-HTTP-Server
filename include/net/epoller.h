#pragma once

#include "base/non_copyable.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <sys/epoll.h>
#include <vector>

namespace hp::net {

class Epoller final : private base::NonCopyable {
public:
    explicit Epoller(std::size_t initial_capacity = 64);
    ~Epoller() noexcept;

    Epoller(Epoller&&) = delete;
    Epoller& operator=(Epoller&&) = delete;

    [[nodiscard]] int fd() const noexcept;

    void add(int observed_fd, std::uint32_t events, std::uint64_t token);
    void modify(int observed_fd, std::uint32_t events, std::uint64_t token);
    void remove(int observed_fd) noexcept;
    [[nodiscard]] std::span<const epoll_event> wait(int timeout_ms);

private:
    void control(int operation, int observed_fd, std::uint32_t events,
                 std::uint64_t token);

    int fd_{-1};
    std::vector<epoll_event> events_;
    std::size_t ready_count_{0};
};

}  // namespace hp::net
