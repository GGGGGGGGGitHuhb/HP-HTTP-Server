#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <set>
#include <stdexcept>
#include <type_traits>

#include "net/event_loop_thread_pool.h"

using namespace hp::net;
using namespace std::chrono_literals;

namespace hp::net {
struct EventLoopThreadPoolTestAccess {
  static std::size_t outstanding(EventLoopThreadPool& pool, std::size_t index) {
    std::lock_guard lock(pool.mutex_);
    return pool.outstanding_.at(index);
  }
};
}  // namespace hp::net

namespace {
void EventLoopThreadPoolCompleteLoopProbe(EventLoop&) {}

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
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

template <class F>
void WaitUntil(F predicate) {
  const auto end = std::chrono::steady_clock::now() + 3s;
  while (!predicate()) {
    Check(std::chrono::steady_clock::now() < end, "deadline");
    std::this_thread::yield();
  }
}

std::size_t CountResources(const char* path) {
  return static_cast<std::size_t>(
      std::distance(std::filesystem::directory_iterator(path), {}));
}

void ModesAndRollback() {
  for (std::size_t count : {0, 1, 2}) {
    EventLoopThreadPool pool;
    std::atomic<int> init{}, cleanup{}, executed{};
    std::mutex mutex;
    std::set<std::thread::id> owners;
    Check(!pool.Post(0, &EventLoopThreadPoolCompleteLoopProbe),
          "post before start");
    pool.RequestStop();

    using RecordWorkerCleanupTaskCleanupState = decltype((cleanup));
    struct RecordWorkerCleanupTask {
      RecordWorkerCleanupTaskCleanupState cleanup;
      decltype(auto) RecordWorkerCleanup(std::size_t, EventLoop& loop) const {
        Check(loop.is_in_loop_thread(), "cleanup owner");
        ++cleanup;
      }
    };

    using RecordWorkerInitializationTaskMutexState = decltype((mutex));
    using RecordWorkerInitializationTaskOwnersState = decltype((owners));
    using RecordWorkerInitializationTaskInitState = decltype((init));
    struct RecordWorkerInitializationTask {
      RecordWorkerInitializationTaskMutexState mutex;
      RecordWorkerInitializationTaskOwnersState owners;
      RecordWorkerInitializationTaskInitState init;
      decltype(auto) RecordWorkerInitialization(std::size_t,
                                                EventLoop& loop) const {
        Check(loop.is_in_loop_thread(), "init owner");
        std::lock_guard lock(mutex);
        owners.insert(std::this_thread::get_id());
        ++init;
      }
    };
    pool.Start(
        count,
        std::bind(&RecordWorkerInitializationTask::RecordWorkerInitialization,
                  RecordWorkerInitializationTask{mutex, owners, init},
                  std::placeholders::_1,
                  std::placeholders::_2),
        std::bind(&RecordWorkerCleanupTask::RecordWorkerCleanup,
                  RecordWorkerCleanupTask{cleanup},
                  std::placeholders::_1,
                  std::placeholders::_2));
    Check(init == static_cast<int>(count) && owners.size() == count,
          "all ready fixed count");

    using RecordWorkerExecutionTaskExecutedState = decltype((executed));
    struct RecordWorkerExecutionTask {
      RecordWorkerExecutionTaskExecutedState executed;
      decltype(auto) RecordWorkerExecution(EventLoop& loop) const {
        Check(loop.is_in_loop_thread(), "task owner");
        ++executed;
      }
    };
    for (std::size_t i = 0; i < count; ++i)
      Check(
          pool.Post(i,
                    std::bind(&RecordWorkerExecutionTask::RecordWorkerExecution,
                              RecordWorkerExecutionTask{executed},
                              std::placeholders::_1)),
          "post accepted");
    Check(!pool.Post(count, &EventLoopThreadPoolCompleteLoopProbe),
          "invalid index rejects");
    ExpectThrows([&] { pool.Start(count); });
    pool.RequestStop();
    pool.RequestStop();
    pool.Join();
    pool.Join();
    Check(cleanup == static_cast<int>(count) &&
              executed == static_cast<int>(count),
          "stop drains all");
    std::cout << "mode=" << count << " init=" << init << " cleanup=" << cleanup
              << '\n';
  }
  auto fds = CountResources("/proc/self/fd");
  auto threads = CountResources("/proc/self/task");
  EventLoopThreadPool failed;
  std::atomic<int> init{}, cleanup{};

  using RecordRollbackCleanupTaskCleanupState = decltype((cleanup));
  struct RecordRollbackCleanupTask {
    RecordRollbackCleanupTaskCleanupState cleanup;
    decltype(auto) RecordRollbackCleanup(std::size_t, EventLoop&) const {
      ++cleanup;
    }
  };

  using FailSecondInitializationTaskInitState = decltype((init));
  struct FailSecondInitializationTask {
    FailSecondInitializationTaskInitState init;
    decltype(auto) FailSecondInitialization(std::size_t index,
                                            EventLoop&) const {
      ++init;
      if (index == 1) throw std::runtime_error("second init failure");
    }
  };

  ExpectThrows([&] {
    failed.Start(
        2,
        std::bind(&FailSecondInitializationTask::FailSecondInitialization,
                  FailSecondInitializationTask{init},
                  std::placeholders::_1,
                  std::placeholders::_2),
        std::bind(&RecordRollbackCleanupTask::RecordRollbackCleanup,
                  RecordRollbackCleanupTask{cleanup},
                  std::placeholders::_1,
                  std::placeholders::_2));
  });
  WaitUntil([&] { return CountResources("/proc/self/task") == threads; });
  Check(init == 2 && cleanup == 2 && CountResources("/proc/self/fd") == fds,
        "rollback all workers");
  failed.Join();
  std::cout << "second_start_failure: init=" << init << " cleanup=" << cleanup
            << " baseline_restored\n";
}

void Readiness() {
  EventLoopThreadPool pool;
  std::promise<void> entered, release;
  auto gate = release.get_future().share();
  std::atomic<bool> returned{};

  using StartBlockedPoolTaskPoolState = decltype((pool));
  using StartBlockedPoolTaskEnteredState = decltype((entered));
  using StartBlockedPoolTaskGateState = decltype((gate));
  using StartBlockedPoolTaskReturnedState = decltype((returned));
  struct StartBlockedPoolTask {
    StartBlockedPoolTaskPoolState pool;
    StartBlockedPoolTaskEnteredState entered;
    StartBlockedPoolTaskGateState gate;
    StartBlockedPoolTaskReturnedState returned;
    decltype(auto) StartBlockedPool() const {
      using WaitForInitializationReleaseTaskEnteredState = decltype((entered));
      using WaitForInitializationReleaseTaskGateState = decltype((gate));
      struct WaitForInitializationReleaseTask {
        WaitForInitializationReleaseTaskEnteredState entered;
        WaitForInitializationReleaseTaskGateState gate;
        decltype(auto) WaitForInitializationRelease(std::size_t index,
                                                    EventLoop&) const {
          if (index == 1) {
            entered.set_value();
            Check(gate.wait_for(3s) == std::future_status::ready,
                  "init gate timeout");
          }
        }
      };
      pool.Start(
          2,
          std::bind(
              &WaitForInitializationReleaseTask::WaitForInitializationRelease,
              WaitForInitializationReleaseTask{entered, gate},
              std::placeholders::_1,
              std::placeholders::_2));
      returned = true;
    }
  };
  std::thread control(
      std::bind(&StartBlockedPoolTask::StartBlockedPool,
                StartBlockedPoolTask{pool, entered, gate, returned}));
  Check(entered.get_future().wait_for(3s) == std::future_status::ready,
        "init reached");
  Check(!returned && !pool.Post(0, &EventLoopThreadPoolCompleteLoopProbe),
        "pool unpublished before all ready");
  pool.RequestStop();
  release.set_value();
  control.join();
  pool.Join();
  std::cout << "readiness: all_ready_barrier_and_start_stop=verified\n";
}

void Capacity(bool fail) {
  EventLoopThreadPool pool;
  pool.Start(2);
  std::promise<void> entered, release;
  auto gate = release.get_future().share();
  std::atomic<int> executed{}, cancelled{}, reentrant{};

  using WaitForConsumerReleaseTaskExecutedState = decltype((executed));
  using WaitForConsumerReleaseTaskEnteredState = decltype((entered));
  using WaitForConsumerReleaseTaskGateState = decltype((gate));
  using WaitForConsumerReleaseTaskFailState = decltype((fail));
  struct WaitForConsumerReleaseTask {
    WaitForConsumerReleaseTaskExecutedState executed;
    WaitForConsumerReleaseTaskEnteredState entered;
    WaitForConsumerReleaseTaskGateState gate;
    WaitForConsumerReleaseTaskFailState fail;
    decltype(auto) WaitForConsumerRelease(EventLoop&) const {
      ++executed;
      entered.set_value();
      Check(gate.wait_for(3s) == std::future_status::ready, "consumer gate");
      if (fail) throw std::runtime_error("fatal consumer");
    }
  };
  Check(pool.Post(
            0,
            std::bind(&WaitForConsumerReleaseTask::WaitForConsumerRelease,
                      WaitForConsumerReleaseTask{executed, entered, gate, fail},
                      std::placeholders::_1)),
        "blocking task");
  Check(entered.get_future().wait_for(3s) == std::future_status::ready,
        "consumer entered");

  struct Capture {
    EventLoopThreadPool& pool;
    std::atomic<int>& released;
    std::atomic<int>& reentrant;

    ~Capture() {
      ++released;
      pool.RequestStop();
      if (!pool.Post(0, &EventLoopThreadPoolCompleteLoopProbe)) ++reentrant;
    }
  };

  for (int i = 1; i < 1024; ++i) {
    if (fail) {
      auto capture =
          std::shared_ptr<Capture>(new Capture{pool, cancelled, reentrant});

      using RetainCancellationPayloadTaskCaptureState =
          std::remove_cvref_t<decltype(capture)>;
      using RetainCancellationPayloadTaskExecutedState = decltype((executed));
      struct RetainCancellationPayloadTask {
        RetainCancellationPayloadTaskCaptureState capture;
        RetainCancellationPayloadTaskExecutedState executed;
        decltype(auto) RetainCancellationPayload(EventLoop&) const {
          ++executed;
        }
      };
      Check(pool.Post(
                0,
                std::bind(
                    &RetainCancellationPayloadTask::RetainCancellationPayload,
                    RetainCancellationPayloadTask{capture, executed},
                    std::placeholders::_1)),
            "capacity accept cancellation capture");
    } else {
      using RecordAcceptedExecutionTaskExecutedState = decltype((executed));
      struct RecordAcceptedExecutionTask {
        RecordAcceptedExecutionTaskExecutedState executed;
        decltype(auto) RecordAcceptedExecution(EventLoop&) const { ++executed; }
      };
      Check(pool.Post(
                0,
                std::bind(&RecordAcceptedExecutionTask::RecordAcceptedExecution,
                          RecordAcceptedExecutionTask{executed},
                          std::placeholders::_1)),
            "capacity accept");
    }
  }
  const auto peak = EventLoopThreadPoolTestAccess::outstanding(pool, 0);
  Check(peak == 1024 && !pool.Post(0, &EventLoopThreadPoolCompleteLoopProbe),
        "1025 rejected including executing task");
  Check(pool.Post(1, &EventLoopThreadPoolCompleteLoopProbe),
        "per worker independent capacity");
  if (!fail) {
    auto capture =
        std::shared_ptr<Capture>(new Capture{pool, cancelled, reentrant});

    using RetainRejectedPayloadTaskCaptureState =
        std::remove_cvref_t<decltype(capture)>;
    struct RetainRejectedPayloadTask {
      RetainRejectedPayloadTaskCaptureState capture;
      decltype(auto) RetainRejectedPayload(EventLoop&) const {}
    };
    Check(
        !pool.Post(0,
                   std::bind(&RetainRejectedPayloadTask::RetainRejectedPayload,
                             RetainRejectedPayloadTask{capture},
                             std::placeholders::_1)),
        "reentrant reject");
    capture.reset();
  }
  pool.RequestStop();
  release.set_value();
  if (fail)
    ExpectThrows([&] { pool.Join(); });
  else
    pool.Join();
  Check(EventLoopThreadPoolTestAccess::outstanding(pool, 0) == 0,
        "all tickets returned");
  Check(fail ? executed == 1 && cancelled == 1023
             : executed == 1024 && cancelled == 1,
        "execute cancel accounting");
  Check(cancelled == reentrant, "all capture destructors reentered safely");
  std::cout << "capacity: fail=" << fail << " peak=" << peak
            << " executed=" << executed << " captures_released=" << cancelled
            << " reentrant=" << reentrant << " outstanding=0\n";
}
void NamedWorkerFirstError() {
  struct WorkerTarget {
    int init{0}, cleanup{0};
    void Initialize(std::size_t index, EventLoop& loop) {
      Check(index == 0 && loop.is_in_loop_thread(),
            "named pool init index and owner");
      ++init;
      throw std::runtime_error("pool init first");
    }
    void Cleanup(std::size_t index, EventLoop& loop) {
      Check(index == 0 && loop.is_in_loop_thread(),
            "named pool cleanup index and owner");
      ++cleanup;
      throw std::runtime_error("pool cleanup second");
    }
  } target;
  EventLoopThreadPool pool;
  bool caught = false;
  try {
    pool.Start(2,
               std::bind_front(&WorkerTarget::Initialize, &target),
               std::bind_front(&WorkerTarget::Cleanup, &target));
  } catch (const std::runtime_error& error) {
    caught = std::string(error.what()) == "pool init first";
  }
  pool.Join();
  Check(caught && target.init == 1 && target.cleanup == 1,
        "named worker rollback preserves first error without starting later "
        "worker");
}

}  // namespace

int main() {
  try {
    NamedWorkerFirstError();
    ModesAndRollback();
    Readiness();
    Capacity(false);
    Capacity(true);
    std::cout << "event_loop_thread_pool_tests: PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
