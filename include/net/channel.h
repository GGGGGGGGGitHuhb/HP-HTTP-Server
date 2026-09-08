#pragma once
#include <cstdint>
#include <functional>
#include "base/non_copyable.h"

namespace hp::net {
class EventLoop;
// Non-owning observer. Remove before destruction; keep alive until callback returns.
// All operations must run on the loop thread (or before the loop starts).
class Channel final : private base::NonCopyable {
public:
    using Callback = std::function<void(std::uint32_t)>;
    Channel(EventLoop& loop, int fd, Callback callback);
    ~Channel() noexcept;
    void set_interest(std::uint32_t events);
    void remove() noexcept;
    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] std::uint32_t interest() const noexcept { return interest_; }
    [[nodiscard]] std::uint32_t revents() const noexcept { return revents_; }
    [[nodiscard]] std::uint64_t token() const noexcept { return token_; }
    [[nodiscard]] bool registered() const noexcept { return token_ != 0; }
private:
    friend class EventLoop;
    EventLoop& loop_;
    const int fd_;
    Callback callback_;
    std::uint32_t interest_{0};
    std::uint32_t revents_{0};
    std::uint64_t token_{0};
};
}
