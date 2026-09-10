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
#include "timer/timer_queue.h"

namespace hp::net {
class Channel;
struct EventLoopTestAccess;
// Owner-thread registry; task/stop/control requests are synchronized cross-thread entry points.
class EventLoop final : private base::NonCopyable {
  public:
    static constexpr std::size_t task_capacity = 1024;
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
    enum class Control { none, drain, force };
    using Deadline = timer::TimerQueue::TimePoint;
    using ControlCallback = std::function<void(Control, Deadline)>;
    void set_control_callback(ControlCallback callback);
    void request_drain(Deadline deadline);
    void request_force();
    void notify_control();
    bool failed() const;
    using TimerId = timer::TimerQueue::Id;
    TimerId add_timer(timer::TimerQueue::TimePoint deadline, Task task);
    bool reschedule_timer(TimerId id, timer::TimerQueue::TimePoint deadline);
    bool cancel_timer(TimerId id);
    std::size_t timer_count() const;
    [[nodiscard]] bool is_in_loop_thread() const noexcept;
    void update_channel(Channel& channel, std::uint32_t interest);
    void remove_channel(Channel& channel) noexcept;
    void set_after_dispatch(std::function<void()> cleanup);
    [[nodiscard]] const Counters& counters() const noexcept {
        assert(is_in_loop_thread());
        return counters_;
    }

  private:
    friend struct ConnectionTimeoutTestAccess;
    friend struct EventLoopTestAccess;
    friend struct ResourceLimitsTestAccess;
    friend class EventLoopThread;
    bool enqueue(Task& task);
    void release_task(Task& task);
    void release_tasks(std::deque<Task>& tasks);
    static std::uint64_t exchange_next_token_for_test(std::uint64_t value);
    enum class State { Ready, Running, Stopping, Stopped, Failed };
    void require_owner() const;
    void dispatch_control();
    bool timers_allowed();
    void dispatch(std::uint64_t token, std::uint32_t mask);
    void wake_locked();
    void drain_wakeup();
    void fail(std::exception_ptr error);
    Epoller epoller_;
    timer::TimerQueue timers_;
    const std::thread::id owner_{std::this_thread::get_id()};
    std::unordered_map<std::uint64_t, Channel*> channels_;
    std::unordered_map<int, std::uint64_t> fds_;
    std::function<void()> after_dispatch_;
    Counters counters_;
    bool polling_{false};
    bool failure_observed_{false};
    int wake_fd_{-1};
    std::unique_ptr<Channel> wake_channel_;
    mutable std::mutex mutex_;
    ControlCallback control_callback_;
    Control control_{Control::none};
    Deadline control_deadline_{};
    bool control_pending_{false};
    std::deque<Task> tasks_;
    std::size_t outstanding_{0};
    State state_{State::Ready};
    std::exception_ptr failure_;
};
} // namespace hp::net
