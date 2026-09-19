#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <functional>
#include <future>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "net/channel.h"
#include "net/event_loop_thread.h"
#include "timer/timer_queue.h"

using hp::timer::TimerQueue;
using namespace hp::net;
using namespace std::chrono_literals;

namespace hp::timer {
struct TimerQueueTestAccess {
  static auto Exchange(TimerQueue::Id value) {
    return TimerQueue::ExchangeNextIdForTest(value);
  }

  static std::size_t ordered(const TimerQueue& queue) {
    return queue.ordered_.size();
  }
};
}  // namespace hp::timer

namespace {
void TimerQueueCompleteQueuedProbe() {}

thread_local int allocation_countdown = -1;
thread_local bool capture_wait = false;
thread_local int last_wait = -2;

void Check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

template <class F>
void ExpectThrows(F function) {
  bool caught = false;
  try {
    function();
  } catch (const std::exception&) {
    caught = true;
  }
  Check(caught, "expected exception");
}
}  // namespace

extern "C" {
void* __real__Znwm(std::size_t);

void* __wrap__Znwm(std::size_t size) {
  if (allocation_countdown == 0) {
    allocation_countdown = -1;
    throw std::bad_alloc();
  }
  if (allocation_countdown > 0) --allocation_countdown;
  return __real__Znwm(size);
}

int __real_epoll_wait(int, epoll_event*, int, int);

int __wrap_epoll_wait(int fd, epoll_event* events, int count, int timeout) {
  last_wait = timeout;
  return __real_epoll_wait(fd, events, count, capture_wait ? 0 : timeout);
}
}

namespace {
void QueueBoundaries() {
  TimerQueue queue, other;
  const auto zero = TimerQueue::TimePoint{};
  std::vector<int> fired;

  using RecordFirstExpiryTaskFiredState = decltype((fired));
  struct RecordFirstExpiryTask {
    RecordFirstExpiryTaskFiredState fired;
    decltype(auto) RecordFirstExpiry() const { fired.push_back(1); }
  };
  const auto first =
      queue.Add(zero + 10ms,
                std::bind(&RecordFirstExpiryTask::RecordFirstExpiry,
                          RecordFirstExpiryTask{fired}));

  using RecordSecondExpiryTaskFiredState = decltype((fired));
  struct RecordSecondExpiryTask {
    RecordSecondExpiryTaskFiredState fired;
    decltype(auto) RecordSecondExpiry() const { fired.push_back(2); }
  };
  const auto second =
      queue.Add(zero + 10ms,
                std::bind(&RecordSecondExpiryTask::RecordSecondExpiry,
                          RecordSecondExpiryTask{fired}));
  Check(!other.Cancel(first) && !other.Reschedule(first, zero), "foreign id");
  queue.RunDue(zero + 9ms);
  Check(fired.empty(), "not early");
  queue.RunDue(zero + 10ms);
  Check(fired == std::vector<int>({1, 2}), "equal boundary ID order");
  Check(!queue.Cancel(first) && !queue.Cancel(second), "executed ids invalid");
  auto id = queue.Add(zero + 1ms, &TimerQueueCompleteQueuedProbe);
  for (int i = 0; i < 100000; ++i)
    Check(queue.Reschedule(id, zero + std::chrono::milliseconds(i % 500 + 1)),
          "reschedule active");
  Check(
      queue.size() == 1 && hp::timer::TimerQueueTestAccess::ordered(queue) == 1,
      "no stale heap records");
  queue.Reschedule(id, zero + 2ms);
  queue.RunDue(zero + 1ms);
  Check(queue.size() == 1, "rescheduled not early");
  queue.Reschedule(id, zero);
  queue.RunDue(zero);
  Check(queue.size() == 0, "reschedule earlier");
  id = queue.Add(zero, &TimerQueueCompleteQueuedProbe);
  Check(queue.Cancel(id) && !queue.Cancel(id), "cancel repeat");
  auto saved = hp::timer::TimerQueueTestAccess::Exchange(UINT64_MAX);
  Check(queue.Add(zero, &TimerQueueCompleteQueuedProbe) == UINT64_MAX,
        "last id");
  ExpectThrows([&] { queue.Add(zero, &TimerQueueCompleteQueuedProbe); });
  ExpectThrows([&] { other.Add(zero, &TimerQueueCompleteQueuedProbe); });
  queue.Clear();
  hp::timer::TimerQueueTestAccess::Exchange(saved);
  std::cout << "queue_boundaries: fired=" << fired.size()
            << " renewals=100000 active_after=0 exhaustion=latched\n";
}

void AllocationSafety() {
  TimerQueue queue;
  const auto zero = TimerQueue::TimePoint{};
  for (int allocation : {0, 1}) {
    allocation_countdown = allocation;
    ExpectThrows([&] { queue.Add(zero, &TimerQueueCompleteQueuedProbe); });
    allocation_countdown = -1;
    Check(queue.size() == 0 &&
              hp::timer::TimerQueueTestAccess::ordered(queue) == 0,
          "add rollback indices");
  }
  auto id = queue.Add(zero + 10ms, &TimerQueueCompleteQueuedProbe);
  allocation_countdown = 0;
  ExpectThrows([&] { queue.Reschedule(id, zero); });
  allocation_countdown = -1;
  Check(queue.size() == 1 && queue.next_deadline() == zero + 10ms,
        "renewal strong guarantee");
  queue.Clear();
  std::cout
      << "allocation: add_indices=2 reschedule=1 strong_guarantee=verified\n";
}

void Reentrant() {
  TimerQueue queue;
  const auto now = TimerQueue::TimePoint{};
  int fired = 0;
  TimerQueue::Id second = 0, third = 0;

  using RescheduleExpiryTaskFiredState = decltype((fired));
  using RescheduleExpiryTaskQueueState = decltype((queue));
  using RescheduleExpiryTaskSecondState = decltype((second));
  using RescheduleExpiryTaskThirdState = decltype((third));
  using RescheduleExpiryTaskNowState = decltype((now));
  struct RescheduleExpiryTask {
    RescheduleExpiryTaskFiredState fired;
    RescheduleExpiryTaskQueueState queue;
    RescheduleExpiryTaskSecondState second;
    RescheduleExpiryTaskThirdState third;
    RescheduleExpiryTaskNowState now;
    decltype(auto) RescheduleExpiry() const {
      ++fired;
      Check(queue.Cancel(second), "same batch cancel");
      Check(queue.Reschedule(third, now + 1ms), "same batch reschedule");

      using RecordNestedExpiryTaskFiredState = decltype((fired));
      struct RecordNestedExpiryTask {
        RecordNestedExpiryTaskFiredState fired;
        decltype(auto) RecordNestedExpiry() const { ++fired; }
      };
      queue.Add(now,
                std::bind(&RecordNestedExpiryTask::RecordNestedExpiry,
                          RecordNestedExpiryTask{fired}));
    }
  };
  queue.Add(now,
            std::bind(&RescheduleExpiryTask::RescheduleExpiry,
                      RescheduleExpiryTask{fired, queue, second, third, now}));

  using RecordCancelledExpiryTaskFiredState = decltype((fired));
  struct RecordCancelledExpiryTask {
    RecordCancelledExpiryTaskFiredState fired;
    decltype(auto) RecordCancelledExpiry() const { fired += 100; }
  };
  second =
      queue.Add(now,
                std::bind(&RecordCancelledExpiryTask::RecordCancelledExpiry,
                          RecordCancelledExpiryTask{fired}));

  using RecordRescheduledExpiryTaskFiredState = decltype((fired));
  struct RecordRescheduledExpiryTask {
    RecordRescheduledExpiryTaskFiredState fired;
    decltype(auto) RecordRescheduledExpiry() const { ++fired; }
  };
  third =
      queue.Add(now,
                std::bind(&RecordRescheduledExpiryTask::RecordRescheduledExpiry,
                          RecordRescheduledExpiryTask{fired}));
  queue.RunDue(now);
  Check(fired == 1 && queue.size() == 2, "same batch revalidated/new deferred");
  queue.RunDue(now);
  Check(fired == 2, "new runs next");
  queue.RunDue(now + 1ms);
  Check(fired == 3, "renewed runs later");

  struct Capture {
    TimerQueue& queue;
    int& released;
    bool clearing;

    ~Capture() {
      ++released;
      if (clearing)
        ExpectThrows([&] { queue.Add({}, &TimerQueueCompleteQueuedProbe); });
      else
        queue.Add({}, &TimerQueueCompleteQueuedProbe);
    }
  };

  int released = 0;
  auto capture = std::shared_ptr<Capture>(new Capture{queue, released, false});

  using RetainCancelledTimerTaskCaptureState =
      std::remove_cvref_t<decltype(capture)>;
  struct RetainCancelledTimerTask {
    RetainCancelledTimerTaskCaptureState capture;
    decltype(auto) RetainCancelledTimer() const {}
  };
  auto id = queue.Add(now,
                      std::bind(&RetainCancelledTimerTask::RetainCancelledTimer,
                                RetainCancelledTimerTask{capture}));
  capture.reset();
  queue.Cancel(id);
  Check(released == 1 && queue.size() == 1, "cancel destructor reentry");
  capture.reset(new Capture{queue, released, true});

  using RetainPendingTimerTaskCaptureState =
      std::remove_cvref_t<decltype(capture)>;
  struct RetainPendingTimerTask {
    RetainPendingTimerTaskCaptureState capture;
    decltype(auto) RetainPendingTimer() const {}
  };
  queue.Add(now,
            std::bind(&RetainPendingTimerTask::RetainPendingTimer,
                      RetainPendingTimerTask{capture}));
  capture.reset();
  queue.Clear();
  Check(released == 2 && queue.size() == 0, "clear rejects destructor add");

  struct ThrowTimerFailureTask {
    decltype(auto) ThrowTimerFailure() const {
      throw std::runtime_error("timer original");
    }
  };
  queue.Add(now,
            std::bind(&ThrowTimerFailureTask::ThrowTimerFailure,
                      ThrowTimerFailureTask{}));
  queue.Add(now, &TimerQueueCompleteQueuedProbe);
  ExpectThrows([&] { queue.RunDue(now); });
  queue.Clear();
  std::cout << "reentrant: fired=" << fired << " capture_release=" << released
            << '\n';
}

void TestEventLoopTimers() {
  EventLoop loop;
  capture_wait = true;
  loop.PollOnce(0);
  Check(last_wait == 0, "poll zero");
  loop.PollOnce(17);
  Check(last_wait == 17, "caller limit");
  loop.PollOnce(-1);
  Check(last_wait == 1000, "fault fallback");
  auto id = loop.AddTimer(TimerQueue::Clock::now() + 1500us,
                          &TimerQueueCompleteQueuedProbe);
  loop.PollOnce(-1);
  Check(last_wait >= 1 && last_wait <= 2, "ceil milliseconds");
  loop.CancelTimer(id);
  capture_wait = false;
  int cleanup = 0, fired = 0;
  struct CleanupCounter {
    int& count;
    void DrainClosedConnections() { ++count; }
  } cleanup_target{cleanup};
  loop.set_DrainClosedConnections_callback(
      std::bind_front(&CleanupCounter::DrainClosedConnections,
                      &cleanup_target));
  auto deadline = TimerQueue::Clock::now() + 100ms;

  using RecordDeadlineExpiryTaskDeadlineState = decltype((deadline));
  using RecordDeadlineExpiryTaskFiredState = decltype((fired));
  struct RecordDeadlineExpiryTask {
    RecordDeadlineExpiryTaskDeadlineState deadline;
    RecordDeadlineExpiryTaskFiredState fired;
    decltype(auto) RecordDeadlineExpiry() const {
      Check(TimerQueue::Clock::now() >= deadline, "no early firing");
      ++fired;
    }
  };
  loop.AddTimer(deadline,
                std::bind(&RecordDeadlineExpiryTask::RecordDeadlineExpiry,
                          RecordDeadlineExpiryTask{deadline, fired}));
  loop.PollOnce(-1);
  Check(fired == 1 && cleanup == 1, "pure timer after_dispatch");
  int fd = ::eventfd(1, EFD_NONBLOCK);

  using RecordOldExpiryTaskFiredState = decltype((fired));
  struct RecordOldExpiryTask {
    RecordOldExpiryTaskFiredState fired;
    decltype(auto) RecordOldExpiry() const { fired += 100; }
  };
  auto old = loop.AddTimer(TimerQueue::Clock::now(),
                           std::bind(&RecordOldExpiryTask::RecordOldExpiry,
                                     RecordOldExpiryTask{fired}));
  Channel channel(loop, fd);
  using HandleConnectionEventObserver1State0 = decltype((loop));
  using HandleConnectionEventObserver1State1 = decltype((old));
  using HandleConnectionEventObserver1State2 = decltype((channel));
  struct HandleConnectionEventObserver1 {
    HandleConnectionEventObserver1State0 loop;
    HandleConnectionEventObserver1State1 old;
    HandleConnectionEventObserver1State2 channel;
    void HandleConnectionEvent(std::uint32_t) {
      loop.RescheduleTimer(old, TimerQueue::Clock::now() + 1h);
      channel.Remove();
    }
  };
  channel.set_HandleConnectionEvent_callback(
      std::bind_front(&HandleConnectionEventObserver1::HandleConnectionEvent,
                      HandleConnectionEventObserver1{loop, old, channel}));
  channel.set_interest(EPOLLIN);
  loop.PollOnce(0);
  Check(fired == 1, "same poll IO renews before due");
  loop.CancelTimer(old);
  ::close(fd);

  using ScheduleNestedExpiryTaskLoopState = decltype((loop));
  using ScheduleNestedExpiryTaskFiredState = decltype((fired));
  struct ScheduleNestedExpiryTask {
    ScheduleNestedExpiryTaskLoopState loop;
    ScheduleNestedExpiryTaskFiredState fired;
    decltype(auto) ScheduleNestedExpiry() const {
      using RecordNestedExpiryTaskFiredState = decltype((fired));
      struct RecordNestedExpiryTask {
        RecordNestedExpiryTaskFiredState fired;
        decltype(auto) RecordNestedExpiry() const { ++fired; }
      };
      loop.AddTimer(TimerQueue::Clock::now(),
                    std::bind(&RecordNestedExpiryTask::RecordNestedExpiry,
                              RecordNestedExpiryTask{fired}));
    }
  };
  loop.QueueInLoop(std::bind(&ScheduleNestedExpiryTask::ScheduleNestedExpiry,
                             ScheduleNestedExpiryTask{loop, fired}));
  loop.PollOnce(0);
  Check(fired == 1, "new timer from task next poll");
  loop.PollOnce(0);
  Check(fired == 2, "next poll timer");

  using RecordDueExpiryTaskFiredState = decltype((fired));
  struct RecordDueExpiryTask {
    RecordDueExpiryTaskFiredState fired;
    decltype(auto) RecordDueExpiry() const { ++fired; }
  };
  loop.AddTimer(TimerQueue::Clock::now(),
                std::bind(&RecordDueExpiryTask::RecordDueExpiry,
                          RecordDueExpiryTask{fired}));
  loop.RequestStop();
  loop.Loop();
  Check(fired == 2 && loop.timer_count() == 0, "stop cancels timers");
  ExpectThrows([&] { loop.AddTimer({}, &TimerQueueCompleteQueuedProbe); });
  std::cout << "event_loop: fired=" << fired
            << " pure_timer_cleanup=1 IO_first=verified\n";
}

void Fatal() {
  EventLoopThread worker;
  int cleanup = 0;

  using RecordTimerCleanupTaskCleanupState = decltype((cleanup));
  struct RecordTimerCleanupTask {
    RecordTimerCleanupTaskCleanupState cleanup;
    decltype(auto) RecordTimerCleanup(EventLoop& loop) const {
      Check(loop.timer_count() == 0, "fatal clears timers before cleanup");
      ++cleanup;
    }
  };

  struct ScheduleFatalTimersTask {
    decltype(auto) ScheduleFatalTimers(EventLoop& loop) const {
      struct ThrowFatalTimerTask {
        decltype(auto) ThrowFatalTimer() const {
          throw std::runtime_error("fatal timer");
        }
      };
      loop.AddTimer(TimerQueue::Clock::now(),
                    std::bind(&ThrowFatalTimerTask::ThrowFatalTimer,
                              ThrowFatalTimerTask{}));
      loop.AddTimer(TimerQueue::Clock::now() + 1h,
                    &TimerQueueCompleteQueuedProbe);
    }
  };
  worker.Start(std::bind(&ScheduleFatalTimersTask::ScheduleFatalTimers,
                         ScheduleFatalTimersTask{},
                         std::placeholders::_1),
               std::bind(&RecordTimerCleanupTask::RecordTimerCleanup,
                         RecordTimerCleanupTask{cleanup},
                         std::placeholders::_1));
  ExpectThrows([&] { worker.Join(); });
  Check(cleanup == 1, "fatal cleanup");
}
void TimerExceptionCleanupOrder() {
  for (int mode : {0, 1, 2}) {
    EventLoop loop;
    std::vector<int> order;
    struct CleanupTarget {
      std::vector<int>& order;
      bool fail;
      void DrainClosedConnections() {
        order.push_back(2);
        if (fail) throw std::runtime_error("timer cleanup error");
      }
    } target{order, mode != 0};
    loop.set_DrainClosedConnections_callback(
        std::bind_front(&CleanupTarget::DrainClosedConnections, &target));
    struct TimerFailureTask {
      std::vector<int>& order;
      int mode;
      void RecordTimerFailure() const {
        order.push_back(1);
        if (mode != 1) throw std::runtime_error("timer task error");
      }
    };
    loop.AddTimer(TimerQueue::Clock::now(),
                  std::bind_front(&TimerFailureTask::RecordTimerFailure,
                                  TimerFailureTask{order, mode}));
    bool caught = false;
    try {
      loop.PollOnce(0);
    } catch (const std::runtime_error& error) {
      caught = true;
      Check(std::string(error.what()) ==
                (mode == 0 ? "timer task error" : "timer cleanup error"),
            "timer preserves task/cleanup exception precedence");
    }
    Check(
        caught && order == std::vector<int>({1, 2}) && loop.timer_count() == 0,
        "timer exceptional path runs cleanup once after task");
  }
  std::cout << "timer_exception task/cleanup/both=3 cleanup_once=verified\n";
}

}  // namespace

int main() {
  try {
    QueueBoundaries();
    AllocationSafety();
    Reentrant();
    TestEventLoopTimers();
    Fatal();
    TimerExceptionCleanupOrder();
    std::cout << "timer_queue_tests: PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
