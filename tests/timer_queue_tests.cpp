#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "net/channel.h"
#include "net/event_loop_thread.h"
#include "timer/timer_queue.h"

using hp::timer::TimerQueue;
using namespace hp::net;
using namespace std::chrono_literals;

namespace hp::timer {
struct TimerQueueTestAccess {
  static auto exchange(TimerQueue::Id value) {
    return TimerQueue::ExchangeNextIdForTest(value);
  }

  static std::size_t ordered(const TimerQueue& queue) {
    return queue.ordered_.size();
  }
};
}  // namespace hp::timer

namespace {
thread_local int allocation_countdown = -1;
thread_local bool capture_wait = false;
thread_local int last_wait = -2;

void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

template <class F>
void throws(F function) {
  bool caught = false;
  try {
    function();
  } catch (const std::exception&) {
    caught = true;
  }
  check(caught, "expected exception");
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
void queue_boundaries() {
  TimerQueue queue, other;
  const auto zero = TimerQueue::TimePoint{};
  std::vector<int> fired;
  const auto first = queue.Add(zero + 10ms, [&] { fired.push_back(1); });
  const auto second = queue.Add(zero + 10ms, [&] { fired.push_back(2); });
  check(!other.Cancel(first) && !other.Reschedule(first, zero), "foreign id");
  queue.RunDue(zero + 9ms);
  check(fired.empty(), "not early");
  queue.RunDue(zero + 10ms);
  check(fired == std::vector<int>({1, 2}), "equal boundary ID order");
  check(!queue.Cancel(first) && !queue.Cancel(second), "executed ids invalid");
  auto id = queue.Add(zero + 1ms, [] {});
  for (int i = 0; i < 100000; ++i)
    check(queue.Reschedule(id, zero + std::chrono::milliseconds(i % 500 + 1)),
          "reschedule active");
  check(
      queue.size() == 1 && hp::timer::TimerQueueTestAccess::ordered(queue) == 1,
      "no stale heap records");
  queue.Reschedule(id, zero + 2ms);
  queue.RunDue(zero + 1ms);
  check(queue.size() == 1, "rescheduled not early");
  queue.Reschedule(id, zero);
  queue.RunDue(zero);
  check(queue.size() == 0, "reschedule earlier");
  id = queue.Add(zero, [] {});
  check(queue.Cancel(id) && !queue.Cancel(id), "cancel repeat");
  auto saved = hp::timer::TimerQueueTestAccess::exchange(UINT64_MAX);
  check(queue.Add(zero, [] {}) == UINT64_MAX, "last id");
  throws([&] { queue.Add(zero, [] {}); });
  throws([&] { other.Add(zero, [] {}); });
  queue.Clear();
  hp::timer::TimerQueueTestAccess::exchange(saved);
  std::cout << "queue_boundaries: fired=" << fired.size()
            << " renewals=100000 active_after=0 exhaustion=latched\n";
}

void allocation_safety() {
  TimerQueue queue;
  const auto zero = TimerQueue::TimePoint{};
  for (int allocation : {0, 1}) {
    allocation_countdown = allocation;
    throws([&] { queue.Add(zero, [] {}); });
    allocation_countdown = -1;
    check(queue.size() == 0 &&
              hp::timer::TimerQueueTestAccess::ordered(queue) == 0,
          "add rollback indices");
  }
  auto id = queue.Add(zero + 10ms, [] {});
  allocation_countdown = 0;
  throws([&] { queue.Reschedule(id, zero); });
  allocation_countdown = -1;
  check(queue.size() == 1 && queue.next_deadline() == zero + 10ms,
        "renewal strong guarantee");
  queue.Clear();
  std::cout
      << "allocation: add_indices=2 reschedule=1 strong_guarantee=verified\n";
}

void reentrant() {
  TimerQueue queue;
  const auto now = TimerQueue::TimePoint{};
  int fired = 0;
  TimerQueue::Id second = 0, third = 0;
  queue.Add(now, [&] {
    ++fired;
    check(queue.Cancel(second), "same batch cancel");
    check(queue.Reschedule(third, now + 1ms), "same batch reschedule");
    queue.Add(now, [&] { ++fired; });
  });
  second = queue.Add(now, [&] { fired += 100; });
  third = queue.Add(now, [&] { ++fired; });
  queue.RunDue(now);
  check(fired == 1 && queue.size() == 2, "same batch revalidated/new deferred");
  queue.RunDue(now);
  check(fired == 2, "new runs next");
  queue.RunDue(now + 1ms);
  check(fired == 3, "renewed runs later");

  struct Capture {
    TimerQueue& queue;
    int& released;
    bool clearing;

    ~Capture() {
      ++released;
      if (clearing)
        throws([&] { queue.Add({}, [] {}); });
      else
        queue.Add({}, [] {});
    }
  };

  int released = 0;
  auto capture = std::shared_ptr<Capture>(new Capture{queue, released, false});
  auto id = queue.Add(now, [capture] {});
  capture.reset();
  queue.Cancel(id);
  check(released == 1 && queue.size() == 1, "cancel destructor reentry");
  capture.reset(new Capture{queue, released, true});
  queue.Add(now, [capture] {});
  capture.reset();
  queue.Clear();
  check(released == 2 && queue.size() == 0, "clear rejects destructor add");
  queue.Add(now, [] { throw std::runtime_error("timer original"); });
  queue.Add(now, [] {});
  throws([&] { queue.RunDue(now); });
  queue.Clear();
  std::cout << "reentrant: fired=" << fired << " capture_release=" << released
            << '\n';
}

void event_loop() {
  EventLoop loop;
  capture_wait = true;
  loop.PollOnce(0);
  check(last_wait == 0, "poll zero");
  loop.PollOnce(17);
  check(last_wait == 17, "caller limit");
  loop.PollOnce(-1);
  check(last_wait == 1000, "fault fallback");
  auto id = loop.AddTimer(TimerQueue::Clock::now() + 1500us, [] {});
  loop.PollOnce(-1);
  check(last_wait >= 1 && last_wait <= 2, "ceil milliseconds");
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
  loop.AddTimer(deadline, [&] {
    check(TimerQueue::Clock::now() >= deadline, "no early firing");
    ++fired;
  });
  loop.PollOnce(-1);
  check(fired == 1 && cleanup == 1, "pure timer after_dispatch");
  int fd = ::eventfd(1, EFD_NONBLOCK);
  auto old = loop.AddTimer(TimerQueue::Clock::now(), [&] { fired += 100; });
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
  check(fired == 1, "same poll IO renews before due");
  loop.CancelTimer(old);
  ::close(fd);
  loop.QueueInLoop(
      [&] { loop.AddTimer(TimerQueue::Clock::now(), [&] { ++fired; }); });
  loop.PollOnce(0);
  check(fired == 1, "new timer from task next poll");
  loop.PollOnce(0);
  check(fired == 2, "next poll timer");
  loop.AddTimer(TimerQueue::Clock::now(), [&] { ++fired; });
  loop.RequestStop();
  loop.Loop();
  check(fired == 2 && loop.timer_count() == 0, "stop cancels timers");
  throws([&] { loop.AddTimer({}, [] {}); });
  std::cout << "event_loop: fired=" << fired
            << " pure_timer_cleanup=1 IO_first=verified\n";
}

void fatal() {
  EventLoopThread worker;
  int cleanup = 0;
  worker.Start(
      [](EventLoop& loop) {
        loop.AddTimer(TimerQueue::Clock::now(),
                      [] { throw std::runtime_error("fatal timer"); });
        loop.AddTimer(TimerQueue::Clock::now() + 1h, [] {});
      },
      [&](EventLoop& loop) {
        check(loop.timer_count() == 0, "fatal clears timers before cleanup");
        ++cleanup;
      });
  throws([&] { worker.Join(); });
  check(cleanup == 1, "fatal cleanup");
}
void timer_exception_cleanup_order() {
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
      check(std::string(error.what()) ==
                (mode == 0 ? "timer task error" : "timer cleanup error"),
            "timer preserves task/cleanup exception precedence");
    }
    check(
        caught && order == std::vector<int>({1, 2}) && loop.timer_count() == 0,
        "timer exceptional path runs cleanup once after task");
  }
  std::cout << "timer_exception task/cleanup/both=3 cleanup_once=verified\n";
}

}  // namespace

int main() {
  try {
    queue_boundaries();
    allocation_safety();
    reentrant();
    event_loop();
    fatal();
    timer_exception_cleanup_order();
    std::cout << "timer_queue_tests: PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
