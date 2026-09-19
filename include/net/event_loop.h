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

// Owner-thread registry; task/stop/control requests are synchronized
// cross-thread entry points.
class EventLoop final : private base::NonCopyable {
 public:
  static constexpr std::size_t kTaskCapacity = 1024;

  using LoopTask = std::function<void()>;

  struct Counters {
    std::size_t adds{}, mods{}, removes{}, dispatches{}, stale{};
  };

  EventLoop();
  ~EventLoop() noexcept;

  void Loop();
  void PollOnce(int timeout_ms);

  bool QueueInLoop(LoopTask task);
  void RequestStop();

  enum class Control { kNone, kDrain, kForce };
  using Deadline = timer::TimerQueue::TimePoint;
  using ControlCallback = std::function<void(Control, Deadline)>;

  void set_HandleControl_callback(ControlCallback callback);
  void set_HandleWorkerControl_callback(ControlCallback callback);
  void RequestDrain(Deadline deadline);
  void RequestForce();
  void NotifyControl();

  bool failed() const;

  using TimerId = timer::TimerQueue::Id;

  TimerId AddTimer(timer::TimerQueue::TimePoint deadline, LoopTask task);
  bool RescheduleTimer(TimerId id, timer::TimerQueue::TimePoint deadline);
  bool CancelTimer(TimerId id);
  std::size_t timer_count() const;

  [[nodiscard]] bool is_in_loop_thread() const noexcept;

  void UpdateChannel(Channel& channel, std::uint32_t interest);
  void RemoveChannel(Channel& channel) noexcept;

  void set_DrainClosedConnections_callback(std::function<void()> cleanup);

  [[nodiscard]] const Counters& counters() const noexcept {
    assert(is_in_loop_thread());
    return counters_;
  }

 private:
  friend struct ConnectionTimeoutTestAccess;
  friend struct EventLoopTestAccess;
  friend struct ResourceLimitsTestAccess;
  friend class EventLoopThread;

  void HandleWakeupEvent(std::uint32_t mask);
  void DispatchTimerTask(const LoopTask& task);
  void InstallControlCallback(ControlCallback callback);

  bool Enqueue(LoopTask& task);
  void ReleaseTask(LoopTask& task);
  void ReleaseTasks(std::deque<LoopTask>& tasks);

  static std::uint64_t ExchangeNextTokenForTest(std::uint64_t value);

  enum class State { kReady, kRunning, kStopping, kStopped, kFailed };

  void RequireOwner() const;

  void DispatchControl();
  bool TimersAllowed();
  void Dispatch(std::uint64_t token, std::uint32_t mask);

  void WakeLocked();
  void DrainWakeup();

  void Fail(std::exception_ptr error);

  Epoller epoller_;
  timer::TimerQueue timers_;
  const std::thread::id owner_{std::this_thread::get_id()};

  std::unordered_map<std::uint64_t, Channel*> channels_;
  std::unordered_map<int, std::uint64_t> fds_;
  std::function<void()> cleanup_after_dispatch_callback_;
  Counters counters_;
  bool polling_{false};
  bool failure_observed_{false};

  int wake_fd_{-1};
  std::unique_ptr<Channel> wake_channel_;

  mutable std::mutex mutex_;

  ControlCallback control_callback_;
  Control control_{Control::kNone};
  Deadline control_deadline_{};
  bool control_pending_{false};

  std::deque<LoopTask> tasks_;
  std::size_t outstanding_{0};

  State state_{State::kReady};
  std::exception_ptr failure_;
};
}  // namespace hp::net
