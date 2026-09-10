#include "timer/timer_queue.h"
#include "net/event_loop_thread.h"
#include "net/channel.h"
#include <atomic>
#include <future>
#include <iostream>
#include <stdexcept>
#include <sys/eventfd.h>
#include <unistd.h>
#include <vector>

using hp::timer::TimerQueue;
using namespace hp::net;
using namespace std::chrono_literals;

namespace hp::timer {
struct TimerQueueTestAccess {
    static auto exchange(TimerQueue::Id value) {
        return TimerQueue::exchange_next_id_for_test(value);
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
    if (!value)
        throw std::runtime_error(message);
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
    if (allocation_countdown > 0)
        --allocation_countdown;
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
    const auto first = queue.add(zero + 10ms, [&] { fired.push_back(1); });
    const auto second = queue.add(zero + 10ms, [&] { fired.push_back(2); });
    check(!other.cancel(first) && !other.reschedule(first, zero), "foreign id");
    queue.run_due(zero + 9ms);
    check(fired.empty(), "not early");
    queue.run_due(zero + 10ms);
    check(fired == std::vector<int>({1, 2}), "equal boundary ID order");
    check(!queue.cancel(first) && !queue.cancel(second),
          "executed ids invalid");
    auto id = queue.add(zero + 1ms, [] {});
    for (int i = 0; i < 100000; ++i)
        check(
            queue.reschedule(id, zero + std::chrono::milliseconds(i % 500 + 1)),
            "reschedule active");
    check(queue.size() == 1 &&
              hp::timer::TimerQueueTestAccess::ordered(queue) == 1,
          "no stale heap records");
    queue.reschedule(id, zero + 2ms);
    queue.run_due(zero + 1ms);
    check(queue.size() == 1, "rescheduled not early");
    queue.reschedule(id, zero);
    queue.run_due(zero);
    check(queue.size() == 0, "reschedule earlier");
    id = queue.add(zero, [] {});
    check(queue.cancel(id) && !queue.cancel(id), "cancel repeat");
    auto saved = hp::timer::TimerQueueTestAccess::exchange(UINT64_MAX);
    check(queue.add(zero, [] {}) == UINT64_MAX, "last id");
    throws([&] { queue.add(zero, [] {}); });
    throws([&] { other.add(zero, [] {}); });
    queue.clear();
    hp::timer::TimerQueueTestAccess::exchange(saved);
    std::cout << "queue_boundaries: fired=" << fired.size()
              << " renewals=100000 active_after=0 exhaustion=latched\n";
}

void allocation_safety() {
    TimerQueue queue;
    const auto zero = TimerQueue::TimePoint{};
    for (int allocation : {0, 1}) {
        allocation_countdown = allocation;
        throws([&] { queue.add(zero, [] {}); });
        allocation_countdown = -1;
        check(queue.size() == 0 &&
                  hp::timer::TimerQueueTestAccess::ordered(queue) == 0,
              "add rollback indices");
    }
    auto id = queue.add(zero + 10ms, [] {});
    allocation_countdown = 0;
    throws([&] { queue.reschedule(id, zero); });
    allocation_countdown = -1;
    check(queue.size() == 1 && queue.next_deadline() == zero + 10ms,
          "renewal strong guarantee");
    queue.clear();
    std::cout
        << "allocation: add_indices=2 reschedule=1 strong_guarantee=verified\n";
}

void reentrant() {
    TimerQueue queue;
    const auto now = TimerQueue::TimePoint{};
    int fired = 0;
    TimerQueue::Id second = 0, third = 0;
    queue.add(now, [&] {
        ++fired;
        check(queue.cancel(second), "same batch cancel");
        check(queue.reschedule(third, now + 1ms), "same batch reschedule");
        queue.add(now, [&] { ++fired; });
    });
    second = queue.add(now, [&] { fired += 100; });
    third = queue.add(now, [&] { ++fired; });
    queue.run_due(now);
    check(fired == 1 && queue.size() == 2,
          "same batch revalidated/new deferred");
    queue.run_due(now);
    check(fired == 2, "new runs next");
    queue.run_due(now + 1ms);
    check(fired == 3, "renewed runs later");

    struct Capture {
        TimerQueue& queue;
        int& released;
        bool clearing;

        ~Capture() {
            ++released;
            if (clearing)
                throws([&] { queue.add({}, [] {}); });
            else
                queue.add({}, [] {});
        }
    };

    int released = 0;
    auto capture =
        std::shared_ptr<Capture>(new Capture{queue, released, false});
    auto id = queue.add(now, [capture] {});
    capture.reset();
    queue.cancel(id);
    check(released == 1 && queue.size() == 1, "cancel destructor reentry");
    capture.reset(new Capture{queue, released, true});
    queue.add(now, [capture] {});
    capture.reset();
    queue.clear();
    check(released == 2 && queue.size() == 0, "clear rejects destructor add");
    queue.add(now, [] { throw std::runtime_error("timer original"); });
    queue.add(now, [] {});
    throws([&] { queue.run_due(now); });
    queue.clear();
    std::cout << "reentrant: fired=" << fired << " capture_release=" << released
              << '\n';
}

void event_loop() {
    EventLoop loop;
    capture_wait = true;
    loop.poll_once(0);
    check(last_wait == 0, "poll zero");
    loop.poll_once(17);
    check(last_wait == 17, "caller limit");
    loop.poll_once(-1);
    check(last_wait == 1000, "fault fallback");
    auto id = loop.add_timer(TimerQueue::Clock::now() + 1500us, [] {});
    loop.poll_once(-1);
    check(last_wait >= 1 && last_wait <= 2, "ceil milliseconds");
    loop.cancel_timer(id);
    capture_wait = false;
    int cleanup = 0, fired = 0;
    loop.set_after_dispatch([&] { ++cleanup; });
    auto deadline = TimerQueue::Clock::now() + 100ms;
    loop.add_timer(deadline, [&] {
        check(TimerQueue::Clock::now() >= deadline, "no early firing");
        ++fired;
    });
    loop.poll_once(-1);
    check(fired == 1 && cleanup == 1, "pure timer after_dispatch");
    int fd = ::eventfd(1, EFD_NONBLOCK);
    auto old = loop.add_timer(TimerQueue::Clock::now(), [&] { fired += 100; });
    Channel channel(loop, fd, [&](std::uint32_t) {
        loop.reschedule_timer(old, TimerQueue::Clock::now() + 1h);
        channel.remove();
    });
    channel.set_interest(EPOLLIN);
    loop.poll_once(0);
    check(fired == 1, "same poll IO renews before due");
    loop.cancel_timer(old);
    ::close(fd);
    loop.queue_in_loop(
        [&] { loop.add_timer(TimerQueue::Clock::now(), [&] { ++fired; }); });
    loop.poll_once(0);
    check(fired == 1, "new timer from task next poll");
    loop.poll_once(0);
    check(fired == 2, "next poll timer");
    loop.add_timer(TimerQueue::Clock::now(), [&] { ++fired; });
    loop.request_stop();
    loop.loop();
    check(fired == 2 && loop.timer_count() == 0, "stop cancels timers");
    throws([&] { loop.add_timer({}, [] {}); });
    std::cout << "event_loop: fired=" << fired
              << " pure_timer_cleanup=1 IO_first=verified\n";
}

void fatal() {
    EventLoopThread worker;
    int cleanup = 0;
    worker.start(
        [](EventLoop& loop) {
            loop.add_timer(TimerQueue::Clock::now(),
                           [] { throw std::runtime_error("fatal timer"); });
            loop.add_timer(TimerQueue::Clock::now() + 1h, [] {});
        },
        [&](EventLoop& loop) {
            check(loop.timer_count() == 0,
                  "fatal clears timers before cleanup");
            ++cleanup;
        });
    throws([&] { worker.join(); });
    check(cleanup == 1, "fatal cleanup");
}
}  // namespace

int main() {
    try {
        queue_boundaries();
        allocation_safety();
        reentrant();
        event_loop();
        fatal();
        std::cout << "timer_queue_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
