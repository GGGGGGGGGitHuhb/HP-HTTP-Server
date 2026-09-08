#pragma once
#include <functional>
#include <unordered_map>
#include "net/epoller.h"

namespace hp::net {
class Channel;
struct EventLoopTestAccess;
// Single-threaded, non-owning registry. Owners remove all Channels before teardown.
class EventLoop final : private base::NonCopyable {
public:
    struct Counters { std::size_t adds{}, mods{}, removes{}, dispatches{}, stale{}; };
    EventLoop() = default;
    ~EventLoop() noexcept;
    [[noreturn]] void loop();
    void poll_once(int timeout_ms);
    void update_channel(Channel& channel, std::uint32_t interest);
    void remove_channel(Channel& channel) noexcept;
    // Owner cleanup only: called after each callback, including exceptional exits.
    void set_after_dispatch(std::function<void()> cleanup);
    [[nodiscard]] const Counters& counters() const noexcept { return counters_; }
private:
    friend struct EventLoopTestAccess;
    void dispatch(std::uint64_t token, std::uint32_t mask);
    Epoller epoller_;
    std::unordered_map<std::uint64_t, Channel*> channels_;
    std::unordered_map<int, std::uint64_t> fds_;
    std::function<void()> after_dispatch_;
    Counters counters_;
    bool polling_{false};
};
}
