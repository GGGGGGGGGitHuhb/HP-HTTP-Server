#pragma once
#include <cassert>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "net/Epoller.h"
#include "timer/TimerQueue.h"

namespace hp::net {
class Channel;
struct EventLoopTestAccess;

// 由所属线程管理的注册表；任务、停止和控制请求是已同步的
// 跨线程入口。
class EventLoop final : private base::NonCopyable {
 public:
  static constexpr std::size_t kTaskCapacity = 1024;

  using TaskCallback = std::function<void()>;

  struct Counters {
    std::size_t adds{}, mods{}, removes{}, dispatches{}, stale{};
  };

  EventLoop();
  ~EventLoop() noexcept;

  void runEventLoop();
  void pollOnce(int timeoutMs);

  bool queueTaskInEventLoop(TaskCallback task);
  void requestLoopStop();

  enum class Control { kNone, kDrain, kForce };
  using Deadline = timer::TimerQueue::TimePoint;
  using ControlCallback = std::function<void(Control, Deadline)>;

  void registerControlCallback(ControlCallback controlCallback);
  void requestLoopDrain(Deadline deadline);
  void requestLoopForceClose();
  void notifyControl();

  bool failed() const;

  using TimerId = timer::TimerQueue::Id;

  TimerId addTimer(timer::TimerQueue::TimePoint deadline, TaskCallback task);
  bool rescheduleTimer(TimerId id, timer::TimerQueue::TimePoint deadline);
  bool cancelTimer(TimerId id);
  std::size_t timerCount() const;

  [[nodiscard]] bool isInLoopThread() const noexcept;

  void updateChannel(Channel& channel, std::uint32_t interest);
  void removeChannel(Channel& channel) noexcept;

  using CleanupCallback = std::function<void()>;
  void registerCleanupCallback(CleanupCallback cleanupCallback);

  [[nodiscard]] const Counters& counters() const noexcept {
    assert(isInLoopThread());
    return counters_;
  }

 private:
  friend struct ConnectionTimeoutTestAccess;
  friend struct EventLoopTestAccess;
  friend struct ResourceLimitsTestAccess;
  friend class EventLoopThread;

  void handleWakeupEvent(std::uint32_t mask);
  void dispatchTimerTask(const TaskCallback& task);
  void setControlCallback(ControlCallback controlCallback) {
    controlCallback_ = std::move(controlCallback);
  }

  void setCleanupCallback(CleanupCallback cleanupCallback) {
    cleanupCallback_ = std::move(cleanupCallback);
  }

  bool enqueueLoopTask(TaskCallback& task);
  void releaseTask(TaskCallback& task);
  void releaseTasks(std::deque<TaskCallback>& tasks);

  static std::uint64_t exchangeNextTokenForTest(std::uint64_t value);

  enum class State { kReady, kRunning, kStopping, kStopped, kFailed };

  void requireOwner() const;

  void dispatchControl();
  bool timersAllowed();
  void dispatchChannelEvent(std::uint64_t token, std::uint32_t mask);

  void wakeLocked();
  void drainWakeup();

  void failEventLoop(std::exception_ptr error);

  Epoller epoller_;
  timer::TimerQueue timers_;
  const std::thread::id owner_{std::this_thread::get_id()};

  std::unordered_map<std::uint64_t, Channel*> channels_;
  std::unordered_map<int, std::uint64_t> fds_;
  CleanupCallback cleanupCallback_;  // 绑定 `ConnectionRegistry::onCleanup()`
  Counters counters_;
  bool polling_{false};
  bool failureObserved_{false};

  int wakeFd_{-1};
  std::unique_ptr<Channel> wakeChannel_;

  mutable std::mutex mutex_;

  ControlCallback controlCallback_;  // 绑定 `TcpServer::onControl()`
  Control control_{Control::kNone};
  Deadline controlDeadline_{};
  bool controlPending_{false};

  std::deque<TaskCallback> tasks_;
  std::size_t outstanding_{0};

  State state_{State::kReady};
  std::exception_ptr failure_;
};
}  // namespace hp::net
