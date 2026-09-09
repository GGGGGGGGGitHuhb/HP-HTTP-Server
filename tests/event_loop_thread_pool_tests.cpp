#include "net/event_loop_thread_pool.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <set>
#include <stdexcept>

using namespace hp::net;
using namespace std::chrono_literals;

namespace hp::net {
struct EventLoopThreadPoolTestAccess {
    static std::size_t outstanding(EventLoopThreadPool& pool, std::size_t index) {
        std::lock_guard lock(pool.mutex_);
        return pool.outstanding_.at(index);
    }
};
} // namespace hp::net

namespace {
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <class F> void throws(F function) {
    bool caught = false;
    try {
        function();
    } catch (const std::exception&) {
        caught = true;
    }
    check(caught, "expected exception");
}

template <class F> void until(F predicate) {
    const auto end = std::chrono::steady_clock::now() + 3s;
    while (!predicate()) {
        check(std::chrono::steady_clock::now() < end, "deadline");
        std::this_thread::yield();
    }
}

std::size_t count(const char* path) {
    return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator(path), {}));
}

void modes_and_rollback() {
    for (std::size_t count : {0, 1, 2}) {
        EventLoopThreadPool pool;
        std::atomic<int> init{}, cleanup{}, executed{};
        std::mutex mutex;
        std::set<std::thread::id> owners;
        check(!pool.post(0,
                         [](EventLoop&) {
                         }),
              "post before start");
        pool.request_stop();
        pool.start(
            count,
            [&](std::size_t, EventLoop& loop) {
                check(loop.is_in_loop_thread(), "init owner");
                std::lock_guard lock(mutex);
                owners.insert(std::this_thread::get_id());
                ++init;
            },
            [&](std::size_t, EventLoop& loop) {
                check(loop.is_in_loop_thread(), "cleanup owner");
                ++cleanup;
            });
        check(init == static_cast<int>(count) && owners.size() == count, "all ready fixed count");
        for (std::size_t i = 0; i < count; ++i)
            check(pool.post(i,
                            [&](EventLoop& loop) {
                                check(loop.is_in_loop_thread(), "task owner");
                                ++executed;
                            }),
                  "post accepted");
        check(!pool.post(count,
                         [](EventLoop&) {
                         }),
              "invalid index rejects");
        throws([&] {
            pool.start(count);
        });
        pool.request_stop();
        pool.request_stop();
        pool.join();
        pool.join();
        check(cleanup == static_cast<int>(count) && executed == static_cast<int>(count),
              "stop drains all");
        std::cout << "mode=" << count << " init=" << init << " cleanup=" << cleanup << '\n';
    }
    auto fds = count("/proc/self/fd");
    auto threads = count("/proc/self/task");
    EventLoopThreadPool failed;
    std::atomic<int> init{}, cleanup{};
    throws([&] {
        failed.start(
            2,
            [&](std::size_t index, EventLoop&) {
                ++init;
                if (index == 1)
                    throw std::runtime_error("second init failure");
            },
            [&](std::size_t, EventLoop&) {
                ++cleanup;
            });
    });
    until([&] {
        return count("/proc/self/task") == threads;
    });
    check(init == 2 && cleanup == 2 && count("/proc/self/fd") == fds, "rollback all workers");
    failed.join();
    std::cout << "second_start_failure: init=" << init << " cleanup=" << cleanup
              << " baseline_restored\n";
}

void readiness() {
    EventLoopThreadPool pool;
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    std::atomic<bool> returned{};
    std::thread control([&] {
        pool.start(2, [&](std::size_t index, EventLoop&) {
            if (index == 1) {
                entered.set_value();
                check(gate.wait_for(3s) == std::future_status::ready, "init gate timeout");
            }
        });
        returned = true;
    });
    check(entered.get_future().wait_for(3s) == std::future_status::ready, "init reached");
    check(!returned && !pool.post(0,
                                  [](EventLoop&) {
                                  }),
          "pool unpublished before all ready");
    pool.request_stop();
    release.set_value();
    control.join();
    pool.join();
    std::cout << "readiness: all_ready_barrier_and_start_stop=verified\n";
}

void capacity(bool fail) {
    EventLoopThreadPool pool;
    pool.start(2);
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    std::atomic<int> executed{}, cancelled{}, reentrant{};
    check(pool.post(0,
                    [&](EventLoop&) {
                        ++executed;
                        entered.set_value();
                        check(gate.wait_for(3s) == std::future_status::ready, "consumer gate");
                        if (fail)
                            throw std::runtime_error("fatal consumer");
                    }),
          "blocking task");
    check(entered.get_future().wait_for(3s) == std::future_status::ready, "consumer entered");

    struct Capture {
        EventLoopThreadPool& pool;
        std::atomic<int>& released;
        std::atomic<int>& reentrant;

        ~Capture() {
            ++released;
            pool.request_stop();
            if (!pool.post(0, [](EventLoop&) {
                }))
                ++reentrant;
        }
    };

    for (int i = 1; i < 1024; ++i) {
        if (fail) {
            auto capture = std::shared_ptr<Capture>(new Capture{pool, cancelled, reentrant});
            check(pool.post(0,
                            [&, capture](EventLoop&) {
                                ++executed;
                            }),
                  "capacity accept cancellation capture");
        } else {
            check(pool.post(0,
                            [&](EventLoop&) {
                                ++executed;
                            }),
                  "capacity accept");
        }
    }
    const auto peak = EventLoopThreadPoolTestAccess::outstanding(pool, 0);
    check(peak == 1024 && !pool.post(0,
                                     [](EventLoop&) {
                                     }),
          "1025 rejected including executing task");
    check(pool.post(1,
                    [](EventLoop&) {
                    }),
          "per worker independent capacity");
    if (!fail) {
        auto capture = std::shared_ptr<Capture>(new Capture{pool, cancelled, reentrant});
        check(!pool.post(0,
                         [capture](EventLoop&) {
                         }),
              "reentrant reject");
        capture.reset();
    }
    pool.request_stop();
    release.set_value();
    if (fail)
        throws([&] {
            pool.join();
        });
    else
        pool.join();
    check(EventLoopThreadPoolTestAccess::outstanding(pool, 0) == 0, "all tickets returned");
    check(fail ? executed == 1 && cancelled == 1023 : executed == 1024 && cancelled == 1,
          "execute cancel accounting");
    check(cancelled == reentrant, "all capture destructors reentered safely");
    std::cout << "capacity: fail=" << fail << " peak=" << peak << " executed=" << executed
              << " captures_released=" << cancelled << " reentrant=" << reentrant
              << " outstanding=0\n";
}
} // namespace

int main() {
    try {
        modes_and_rollback();
        readiness();
        capacity(false);
        capacity(true);
        std::cout << "event_loop_thread_pool_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
