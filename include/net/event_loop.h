#pragma once
#include <cassert>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include "net/epoller.h"

namespace hp::net {
class Channel;
struct EventLoopTestAccess;
// Owner-thread registry; only queue_in_loop/request_stop/identity cross threads.
class EventLoop final : private base::NonCopyable {
  public:
    using Task = std::function<void()>;
    struct Counters {
        std::size_t adds{}, mods{}, removes{}, dispatches{}, stale{};
    };
    EventLoop();
    ~EventLoop() noexcept;
    void loop();
    void poll_once(int timeout_ms);
    bool queue_in_loop(Task task);
    void request_stop();
    [[nodiscard]] bool is_in_loop_thread() const noexcept;
    void update_channel(Channel& channel, std::uint32_t interest);
    void remove_channel(Channel& channel) noexcept;
    void set_after_dispatch(std::function<void()> cleanup);
    [[nodiscard]] const Counters& counters() const noexcept {
        assert(is_in_loop_thread());
        return counters_;
    }

  private:
    friend struct EventLoopTestAccess;
    friend class EventLoopThread;
    bool enqueue(Task& task);
    static std::uint64_t exchange_next_token_for_test(std::uint64_t value);
    enum class State { Ready, Running, Stopping, Stopped, Failed };
    void require_owner() const;
    void dispatch(std::uint64_t token, std::uint32_t mask);
    void wake_locked();
    void drain_wakeup();
    void fail(std::exception_ptr error);
    Epoller epoller_;
    const std::thread::id owner_{std::this_thread::get_id()};
    std::unordered_map<std::uint64_t, Channel*> channels_;
    std::unordered_map<int, std::uint64_t> fds_;
    std::function<void()> after_dispatch_;
    Counters counters_;
    bool polling_{false};
    bool failure_observed_{false};
    int wake_fd_{-1};
    std::unique_ptr<Channel> wake_channel_;
    std::mutex mutex_;
    std::deque<Task> tasks_;
    State state_{State::Ready};
    std::exception_ptr failure_;
};
} // namespace hp::net
