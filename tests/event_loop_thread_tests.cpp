#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <climits>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <type_traits>

#include "net/channel.h"
#include "net/event_loop_thread.h"

using namespace hp::net;
using namespace std::chrono_literals;

namespace hp::net {
struct EventLoopTestAccess {
  static void token(std::uint64_t v) { EventLoop::ExchangeNextTokenForTest(v); }
};
}  // namespace hp::net

namespace {
void EventLoopThreadCompleteQueuedProbe() {}
void EventLoopThreadCompleteLoopProbe(EventLoop&) {}

std::atomic<int> create_error{}, register_error{}, read_fault{}, write_fault{};
std::atomic<int> reads{}, writes{}, read_eintr{}, write_eintr{}, read_eagain{},
    write_eagain{}, waits{};

void Check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

template <class F>
void ExpectThrows(F f) {
  bool caught = false;
  try {
    f();
  } catch (const std::exception&) {
    caught = true;
  }
  Check(caught, "expected exception");
}

template <class F>
void WaitUntil(F f) {
  auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!f()) {
    Check(std::chrono::steady_clock::now() < deadline, "deadline");
    std::this_thread::yield();
  }
}

void Blocked(pid_t tid) {
  WaitUntil([&] {
    std::ifstream in("/proc/self/task/" + std::to_string(tid) + "/syscall");
    long call = -1;
    in >> call;
    return call == SYS_epoll_wait;
  });
}

std::size_t CountResources(const char* path) {
  return static_cast<std::size_t>(
      std::distance(std::filesystem::directory_iterator(path), {}));
}
}  // namespace

extern "C" {
int __real_eventfd(unsigned int, int);

int __wrap_eventfd(unsigned int value, int flags) {
  if (create_error.exchange(0)) {
    errno = EMFILE;
    return -1;
  }
  return __real_eventfd(value, flags);
}

int __real_epoll_ctl(int, int, int, epoll_event*);

int __wrap_epoll_ctl(int fd, int op, int observed, epoll_event* event) {
  if (op == EPOLL_CTL_ADD && register_error.exchange(0)) {
    errno = ENOSPC;
    return -1;
  }
  return __real_epoll_ctl(fd, op, observed, event);
}

int __real_epoll_wait(int, epoll_event*, int, int);

int __wrap_epoll_wait(int fd, epoll_event* event, int max, int timeout) {
  ++waits;
  return __real_epoll_wait(fd, event, max, timeout);
}

ssize_t __real_read(int, void*, size_t);

ssize_t __wrap_read(int fd, void* data, size_t size) {
  if (size == 8) {
    ++reads;
    const int fault = read_fault.exchange(0);
    if (fault == 1) {
      ++read_eintr;
      errno = EINTR;
      return -1;
    }
    if (fault == 2) {
      ++read_eagain;
      errno = EAGAIN;
      return -1;
    }
    if (fault == 3) {
      errno = EIO;
      return -1;
    }
    if (fault == 4) return 4;
  }
  auto result = __real_read(fd, data, size);
  if (size == 8 && result < 0 && errno == EAGAIN) ++read_eagain;
  return result;
}

ssize_t __real_write(int, const void*, size_t);

ssize_t __wrap_write(int fd, const void* data, size_t size) {
  if (size == 8) {
    ++writes;
    const int fault = write_fault.exchange(0);
    if (fault == 1) {
      ++write_eintr;
      errno = EINTR;
      return -1;
    }
    // Leave a real pending notification to reproduce saturated eventfd.
    if (fault == 2) {
      __real_write(fd, data, size);
      ++write_eagain;
      errno = EAGAIN;
      return -1;
    }
    if (fault == 3) {
      errno = EIO;
      return -1;
    }
    if (fault == 4) return 4;
  }
  return __real_write(fd, data, size);
}
}

namespace {
void OwnerAndReady() {
  EventLoop loop;
  std::atomic<int> rejected{};
  int channel_fd = ::eventfd(0, EFD_NONBLOCK);
  Channel channel(loop, channel_fd);
  struct HandleConnectionEventObserver1 {
    void HandleConnectionEvent(std::uint32_t) {}
  };
  channel.set_HandleConnectionEvent_callback(
      std::bind_front(&HandleConnectionEventObserver1::HandleConnectionEvent,
                      HandleConnectionEventObserver1{}));

  using ProbeWrongThreadCallsTaskLoopState = decltype((loop));
  using ProbeWrongThreadCallsTaskRejectedState = decltype((rejected));
  using ProbeWrongThreadCallsTaskChannelState = decltype((channel));
  struct ProbeWrongThreadCallsTask {
    ProbeWrongThreadCallsTaskLoopState loop;
    ProbeWrongThreadCallsTaskRejectedState rejected;
    ProbeWrongThreadCallsTaskChannelState channel;
    decltype(auto) ProbeWrongThreadCalls() const {
      try {
        loop.PollOnce(0);
      } catch (const std::logic_error&) {
        ++rejected;
      }
      try {
        loop.set_DrainClosedConnections_callback({});
      } catch (const std::logic_error&) {
        ++rejected;
      }
      try {
        channel.set_interest(EPOLLIN);
      } catch (const std::logic_error&) {
        ++rejected;
      }
    }
  };
  std::thread other(
      std::bind(&ProbeWrongThreadCallsTask::ProbeWrongThreadCalls,
                ProbeWrongThreadCallsTask{loop, rejected, channel}));
  other.join();
  Check(rejected == 3, "owner rejection");
  ::close(channel_fd);
  int n = 0;
  ExpectThrows([&] { loop.QueueInLoop({}); });

  using RecordReadyExecutionTaskNState = decltype((n));
  struct RecordReadyExecutionTask {
    RecordReadyExecutionTaskNState n;
    decltype(auto) RecordReadyExecution() const { ++n; }
  };
  Check(loop.QueueInLoop(
            std::bind(&RecordReadyExecutionTask::RecordReadyExecution,
                      RecordReadyExecutionTask{n})),
        "ready accepts");
  loop.RequestStop();
  loop.RequestStop();
  Check(!loop.QueueInLoop(&EventLoopThreadCompleteQueuedProbe),
        "prestop rejects");
  loop.Loop();
  Check(n == 1, "prestop drains");
  loop.PollOnce(0);
  ExpectThrows([&] { loop.Loop(); });
  std::cout << "owner_ready: rejected=" << rejected << " executed=" << n
            << '\n';
}

void Producers() {
  EventLoopThread worker;
  worker.Start();
  std::barrier gate(5);
  std::vector<std::thread> producers;
  std::vector<int> seen(4000);
  int next[4]{};
  int executed = 0, depth = 0, max_depth = 0;
  std::atomic<int> accepted{};

  using SubmitOrderedTasksTaskPState = std::remove_cvref_t<int>;
  using SubmitOrderedTasksTaskGateState = decltype((gate));
  using SubmitOrderedTasksTaskWorkerState = decltype((worker));
  using SubmitOrderedTasksTaskNextState = decltype((next));
  using SubmitOrderedTasksTaskSeenState = decltype((seen));
  using SubmitOrderedTasksTaskExecutedState = decltype((executed));
  using SubmitOrderedTasksTaskAcceptedState = decltype((accepted));
  struct SubmitOrderedTasksTask {
    SubmitOrderedTasksTaskPState p;
    SubmitOrderedTasksTaskGateState gate;
    SubmitOrderedTasksTaskWorkerState worker;
    SubmitOrderedTasksTaskNextState next;
    SubmitOrderedTasksTaskSeenState seen;
    SubmitOrderedTasksTaskExecutedState executed;
    SubmitOrderedTasksTaskAcceptedState accepted;
    decltype(auto) SubmitOrderedTasks() const {
      gate.arrive_and_wait();
      for (int i = 0; i < 1000; ++i) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);

        using VerifyOrderedExecutionTaskPState =
            std::remove_cvref_t<decltype(p)>;
        using VerifyOrderedExecutionTaskIState =
            std::remove_cvref_t<decltype(i)>;
        using VerifyOrderedExecutionTaskNextState = decltype((next));
        using VerifyOrderedExecutionTaskSeenState = decltype((seen));
        using VerifyOrderedExecutionTaskExecutedState = decltype((executed));
        struct VerifyOrderedExecutionTask {
          VerifyOrderedExecutionTaskPState p;
          VerifyOrderedExecutionTaskIState i;
          VerifyOrderedExecutionTaskNextState next;
          VerifyOrderedExecutionTaskSeenState seen;
          VerifyOrderedExecutionTaskExecutedState executed;
          decltype(auto) VerifyOrderedExecution(EventLoop& loop) const {
            Check(loop.is_in_loop_thread(), "owner execution");
            Check(next[p]++ == i, "producer order");
            ++seen[p * 1000 + i];
            ++executed;
          }
        };
        while (!worker.Post(
            std::bind(&VerifyOrderedExecutionTask::VerifyOrderedExecution,
                      VerifyOrderedExecutionTask{p, i, next, seen, executed},
                      std::placeholders::_1))) {
          Check(std::chrono::steady_clock::now() < deadline,
                "bounded task retry deadline");
          std::this_thread::yield();
        }
        ++accepted;
      }
    }
  };
  for (int p = 0; p < 4; ++p)
    producers.emplace_back(
        std::bind(&SubmitOrderedTasksTask::SubmitOrderedTasks,
                  SubmitOrderedTasksTask{p,
                                         gate,
                                         worker,
                                         next,
                                         seen,
                                         executed,
                                         accepted}));
  gate.arrive_and_wait();
  for (auto& t : producers) t.join();
  std::promise<void> nested;

  using QueueNestedTaskDepthState = decltype((depth));
  using QueueNestedTaskMaxDepthState = decltype((max_depth));
  using QueueNestedTaskNestedState = decltype((nested));
  struct QueueNestedTaskTarget {
    QueueNestedTaskDepthState depth;
    QueueNestedTaskMaxDepthState max_depth;
    QueueNestedTaskNestedState nested;
    decltype(auto) QueueNestedTask(EventLoop& loop) const {
      ++depth;
      max_depth = std::max(max_depth, depth);

      using CompleteNestedTaskDepthState = decltype((depth));
      using CompleteNestedTaskMaxDepthState = decltype((max_depth));
      using CompleteNestedTaskNestedState = decltype((nested));
      struct CompleteNestedTaskTarget {
        CompleteNestedTaskDepthState depth;
        CompleteNestedTaskMaxDepthState max_depth;
        CompleteNestedTaskNestedState nested;
        decltype(auto) CompleteNestedTask() const {
          ++depth;
          max_depth = std::max(max_depth, depth);
          --depth;
          nested.set_value();
        }
      };
      loop.QueueInLoop(
          std::bind(&CompleteNestedTaskTarget::CompleteNestedTask,
                    CompleteNestedTaskTarget{depth, max_depth, nested}));
      --depth;
    }
  };
  worker.Post(std::bind(&QueueNestedTaskTarget::QueueNestedTask,
                        QueueNestedTaskTarget{depth, max_depth, nested},
                        std::placeholders::_1));
  Check(nested.get_future().wait_for(3s) == std::future_status::ready,
        "nested deadline");
  worker.RequestStop();
  worker.Join();
  Check(accepted == 4000 && executed == accepted && max_depth == 1,
        "task accounting/depth");
  for (auto n : seen) Check(n == 1, "unique execution");
  std::cout << "producers: accepted=" << accepted << " executed=" << executed
            << " depth=" << max_depth << '\n';
}

void Wake() {
  EventLoopThread worker;
  pid_t tid{};

  using RecordWorkerTidTaskTidState = decltype((tid));
  struct RecordWorkerTidTask {
    RecordWorkerTidTaskTidState tid;
    decltype(auto) RecordWorkerTid(EventLoop&) const {
      tid = static_cast<pid_t>(::syscall(SYS_gettid));
    }
  };
  worker.Start(std::bind(&RecordWorkerTidTask::RecordWorkerTid,
                         RecordWorkerTidTask{tid},
                         std::placeholders::_1));
  Blocked(tid);
  auto before = waits.load();
  auto start = std::chrono::steady_clock::now();
  std::promise<void> done;
  write_fault = 1;
  read_fault = 1;

  using CompleteWakeProbeTaskDoneState = decltype((done));
  struct CompleteWakeProbeTask {
    CompleteWakeProbeTaskDoneState done;
    decltype(auto) CompleteWakeProbe(EventLoop&) const { done.set_value(); }
  };
  worker.Post(std::bind(&CompleteWakeProbeTask::CompleteWakeProbe,
                        CompleteWakeProbeTask{done},
                        std::placeholders::_1));
  Check(done.get_future().wait_for(500ms) == std::future_status::ready,
        "eventfd post wake");
  Blocked(tid);
  write_fault = 2;
  read_fault = 2;
  std::promise<void> again;

  using CompleteInterruptProbeTaskAgainState = decltype((again));
  struct CompleteInterruptProbeTask {
    CompleteInterruptProbeTaskAgainState again;
    decltype(auto) CompleteInterruptProbe(EventLoop&) const {
      again.set_value();
    }
  };
  worker.Post(std::bind(&CompleteInterruptProbeTask::CompleteInterruptProbe,
                        CompleteInterruptProbeTask{again},
                        std::placeholders::_1));
  Check(again.get_future().wait_for(500ms) == std::future_status::ready,
        "EAGAIN wake");
  Blocked(tid);
  worker.RequestStop();
  worker.Join();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count();
  Check(elapsed < 500 && waits - before < 10,
        "healthy wake before fallback/no spin");
  Check(read_eintr && write_eintr && read_eagain && write_eagain,
        "fault coverage");
  std::cout << "wake: real_epoll_wait_handshake=3 elapsed_ms=" << elapsed
            << " waits=" << waits - before << " reads=" << reads
            << " writes=" << writes << " EINTR=" << read_eintr << '/'
            << write_eintr << " EAGAIN=" << read_eagain << '/' << write_eagain
            << '\n';
}

void StopRace() {
  EventLoopThread worker;
  Check(!worker.Post(&EventLoopThreadCompleteLoopProbe),
        "before start rejects");
  worker.RequestStop();
  worker.Start();
  std::atomic<int> accepted{}, rejected{}, executed{};
  std::barrier gate(5);
  std::vector<std::thread> callers;

  using SubmitRacingTasksTaskGateState = decltype((gate));
  using SubmitRacingTasksTaskWorkerState = decltype((worker));
  using SubmitRacingTasksTaskExecutedState = decltype((executed));
  using SubmitRacingTasksTaskAcceptedState = decltype((accepted));
  using SubmitRacingTasksTaskRejectedState = decltype((rejected));
  struct SubmitRacingTasksTask {
    SubmitRacingTasksTaskGateState gate;
    SubmitRacingTasksTaskWorkerState worker;
    SubmitRacingTasksTaskExecutedState executed;
    SubmitRacingTasksTaskAcceptedState accepted;
    SubmitRacingTasksTaskRejectedState rejected;
    decltype(auto) SubmitRacingTasks() const {
      gate.arrive_and_wait();

      using RecordRacingExecutionTaskExecutedState = decltype((executed));
      struct RecordRacingExecutionTask {
        RecordRacingExecutionTaskExecutedState executed;
        decltype(auto) RecordRacingExecution(EventLoop&) const { ++executed; }
      };
      for (int j = 0; j < 1000; ++j)
        if (worker.Post(
                std::bind(&RecordRacingExecutionTask::RecordRacingExecution,
                          RecordRacingExecutionTask{executed},
                          std::placeholders::_1)))
          ++accepted;
        else
          ++rejected;
    }
  };
  for (int i = 0; i < 4; ++i)
    callers.emplace_back(std::bind(
        &SubmitRacingTasksTask::SubmitRacingTasks,
        SubmitRacingTasksTask{gate, worker, executed, accepted, rejected}));
  gate.arrive_and_wait();
  worker.RequestStop();
  worker.RequestStop();
  for (auto& t : callers) t.join();
  worker.Join();
  worker.Join();
  Check(accepted == executed && accepted + rejected == 4000, "stop accounting");
  Check(!worker.Post(&EventLoopThreadCompleteLoopProbe), "after join rejects");
  ExpectThrows([&] { worker.Start(); });
  std::cout << "stop_race: accepted=" << accepted << " executed=" << executed
            << " rejected=" << rejected << '\n';
}

void Lifecycle() {
  auto fds = CountResources("/proc/self/fd"),
       threads = CountResources("/proc/self/task");
  int cleanup = 0;
  for (int i = 0; i < 100; ++i) {
    EventLoopThread worker;

    using RecordLifecycleCleanupTaskCleanupState = decltype((cleanup));
    struct RecordLifecycleCleanupTask {
      RecordLifecycleCleanupTaskCleanupState cleanup;
      decltype(auto) RecordLifecycleCleanup(EventLoop& loop) const {
        Check(loop.is_in_loop_thread(), "cleanup owner");
        ++cleanup;
      }
    };
    worker.Start({},
                 std::bind(&RecordLifecycleCleanupTask::RecordLifecycleCleanup,
                           RecordLifecycleCleanupTask{cleanup},
                           std::placeholders::_1));
    worker.Post(&EventLoopThreadCompleteLoopProbe);
  }
  WaitUntil([&] { return CountResources("/proc/self/task") == threads; });
  Check(CountResources("/proc/self/fd") == fds && cleanup == 100,
        "resource baseline");
  std::cout << "lifecycle: cycles=" << cleanup << " fd=" << fds << '/'
            << CountResources("/proc/self/fd") << " threads=" << threads << '/'
            << CountResources("/proc/self/task") << '\n';
}

struct PassiveChannelObserver {
  static void HandleConnectionEvent(std::uint32_t) {}
};

struct FailingChannelObserver {
  static void HandleConnectionEvent(std::uint32_t) {
    throw std::runtime_error("IO");
  }
};

void Failures() {
  int cleanup = 0;
  EventLoopThread init_fail;
  int partial_fd = -1;
  std::unique_ptr<Channel> partial_channel;
  auto before_fds = CountResources("/proc/self/fd");

  using CleanPartialInitializationTaskPartialChannelState =
      decltype((partial_channel));
  using CleanPartialInitializationTaskPartialFdState = decltype((partial_fd));
  using CleanPartialInitializationTaskCleanupState = decltype((cleanup));
  struct CleanPartialInitializationTask {
    CleanPartialInitializationTaskPartialChannelState partial_channel;
    CleanPartialInitializationTaskPartialFdState partial_fd;
    CleanPartialInitializationTaskCleanupState cleanup;
    decltype(auto) CleanPartialInitialization(EventLoop& loop) const {
      Check(loop.is_in_loop_thread(), "partial cleanup owner");
      partial_channel->Remove();
      partial_channel.reset();
      ::close(partial_fd);
      ++cleanup;
    }
  };

  using FailPartialInitializationTaskPartialFdState = decltype((partial_fd));
  using FailPartialInitializationTaskPartialChannelState =
      decltype((partial_channel));
  struct FailPartialInitializationTask {
    FailPartialInitializationTaskPartialFdState partial_fd;
    FailPartialInitializationTaskPartialChannelState partial_channel;
    decltype(auto) FailPartialInitialization(EventLoop& loop) const {
      partial_fd = ::eventfd(0, EFD_NONBLOCK);
      partial_channel = std::make_unique<Channel>(loop, partial_fd);
      partial_channel->set_HandleConnectionEvent_callback(
          &PassiveChannelObserver::HandleConnectionEvent);
      partial_channel->set_interest(EPOLLIN);
      throw std::runtime_error("init");
    }
  };

  ExpectThrows([&] {
    init_fail.Start(
        std::bind(&FailPartialInitializationTask::FailPartialInitialization,
                  FailPartialInitializationTask{partial_fd, partial_channel},
                  std::placeholders::_1),
        std::bind(&CleanPartialInitializationTask::CleanPartialInitialization,
                  CleanPartialInitializationTask{partial_channel,
                                                 partial_fd,
                                                 cleanup},
                  std::placeholders::_1));
  });
  init_fail.Join();
  Check(cleanup == 1 && CountResources("/proc/self/fd") == before_fds,
        "partial init cleanup once");
  EventLoopThread worker;
  std::promise<void> entered, release;
  auto gate = release.get_future().share();
  int executed = 0, cancelled = 0;

  struct Capture {
    int& count;
    std::thread::id owner;

    ~Capture() {
      if (std::this_thread::get_id() != owner) std::abort();
      ++count;
    }
  };

  std::thread::id owner;

  using RecordFailureCleanupTaskCleanupState = decltype((cleanup));
  struct RecordFailureCleanupTask {
    RecordFailureCleanupTaskCleanupState cleanup;
    decltype(auto) RecordFailureCleanup(EventLoop&) const { ++cleanup; }
  };

  using RecordWorkerOwnerTaskOwnerState = decltype((owner));
  struct RecordWorkerOwnerTask {
    RecordWorkerOwnerTaskOwnerState owner;
    decltype(auto) RecordWorkerOwner(EventLoop&) const {
      owner = std::this_thread::get_id();
    }
  };
  worker.Start(std::bind(&RecordWorkerOwnerTask::RecordWorkerOwner,
                         RecordWorkerOwnerTask{owner},
                         std::placeholders::_1),
               std::bind(&RecordFailureCleanupTask::RecordFailureCleanup,
                         RecordFailureCleanupTask{cleanup},
                         std::placeholders::_1));

  using ThrowReleasedTaskFailureTaskExecutedState = decltype((executed));
  using ThrowReleasedTaskFailureTaskEnteredState = decltype((entered));
  using ThrowReleasedTaskFailureTaskGateState = decltype((gate));
  struct ThrowReleasedTaskFailureTask {
    ThrowReleasedTaskFailureTaskExecutedState executed;
    ThrowReleasedTaskFailureTaskEnteredState entered;
    ThrowReleasedTaskFailureTaskGateState gate;
    decltype(auto) ThrowReleasedTaskFailure(EventLoop&) const {
      ++executed;
      entered.set_value();
      gate.wait();
      throw std::runtime_error("task");
    }
  };
  worker.Post(std::bind(&ThrowReleasedTaskFailureTask::ThrowReleasedTaskFailure,
                        ThrowReleasedTaskFailureTask{executed, entered, gate},
                        std::placeholders::_1));
  Check(entered.get_future().wait_for(3s) == std::future_status::ready,
        "task entered");
  auto capture = std::shared_ptr<Capture>(new Capture{cancelled, owner});

  using RetainPendingPayloadTaskCaptureState =
      std::remove_cvref_t<decltype(capture)>;
  struct RetainPendingPayloadTask {
    RetainPendingPayloadTaskCaptureState capture;
    decltype(auto) RetainPendingPayload(EventLoop&) const {}
  };
  worker.Post(std::bind(&RetainPendingPayloadTask::RetainPendingPayload,
                        RetainPendingPayloadTask{capture},
                        std::placeholders::_1));
  capture.reset();
  release.set_value();
  ExpectThrows([&] { worker.Join(); });
  worker.Join();
  Check(executed == 1 && cancelled == 1 && cleanup == 2,
        "failure cancellation accounting");
  EventLoopThread self;
  self.Start();

  using RejectSelfJoinTaskSelfState = decltype((self));
  struct RejectSelfJoinTask {
    RejectSelfJoinTaskSelfState self;
    decltype(auto) RejectSelfJoin(EventLoop& loop) const {
      ExpectThrows([&] { self.Join(); });
      loop.RequestStop();
    }
  };
  self.Post(std::bind(&RejectSelfJoinTask::RejectSelfJoin,
                      RejectSelfJoinTask{self},
                      std::placeholders::_1));
  self.Join();
  EventLoopThread bad_cleanup;

  struct ThrowCleanupFailureTask {
    decltype(auto) ThrowCleanupFailure(EventLoop&) const {
      throw std::runtime_error("cleanup");
    }
  };
  bad_cleanup.Start({},
                    std::bind(&ThrowCleanupFailureTask::ThrowCleanupFailure,
                              ThrowCleanupFailureTask{},
                              std::placeholders::_1));
  bad_cleanup.RequestStop();
  ExpectThrows([&] { bad_cleanup.Join(); });
  {
    EventLoopThread unobserved;

    struct ScheduleUnobservedFailureTask {
      decltype(auto) ScheduleUnobservedFailure(EventLoop& loop) const {
        struct ThrowUnobservedFailureTask {
          decltype(auto) ThrowUnobservedFailure() const {
            throw std::runtime_error("expected unobserved worker failure");
          }
        };
        loop.QueueInLoop(
            std::bind(&ThrowUnobservedFailureTask::ThrowUnobservedFailure,
                      ThrowUnobservedFailureTask{}));
      }
    };
    unobserved.Start(
        std::bind(&ScheduleUnobservedFailureTask::ScheduleUnobservedFailure,
                  ScheduleUnobservedFailureTask{},
                  std::placeholders::_1));
  }
  std::cout << "failures: accepted=2 executed=" << executed
            << " cancelled=" << cancelled << " cleanup=" << cleanup << '\n';
}

void StartStop() {
  EventLoopThread worker;
  std::promise<void> init, release;
  auto gate = release.get_future().share();
  std::atomic<bool> returned{};

  using StartBlockedWorkerTaskWorkerState = decltype((worker));
  using StartBlockedWorkerTaskInitState = decltype((init));
  using StartBlockedWorkerTaskGateState = decltype((gate));
  using StartBlockedWorkerTaskReturnedState = decltype((returned));
  struct StartBlockedWorkerTask {
    StartBlockedWorkerTaskWorkerState worker;
    StartBlockedWorkerTaskInitState init;
    StartBlockedWorkerTaskGateState gate;
    StartBlockedWorkerTaskReturnedState returned;
    decltype(auto) StartBlockedWorker() const {
      using WaitForStartReleaseTaskInitState = decltype((init));
      using WaitForStartReleaseTaskGateState = decltype((gate));
      struct WaitForStartReleaseTask {
        WaitForStartReleaseTaskInitState init;
        WaitForStartReleaseTaskGateState gate;
        decltype(auto) WaitForStartRelease(EventLoop&) const {
          init.set_value();
          gate.wait();
        }
      };
      worker.Start(std::bind(&WaitForStartReleaseTask::WaitForStartRelease,
                             WaitForStartReleaseTask{init, gate},
                             std::placeholders::_1));
      returned = true;
    }
  };
  std::thread control(
      std::bind(&StartBlockedWorkerTask::StartBlockedWorker,
                StartBlockedWorkerTask{worker, init, gate, returned}));
  Check(init.get_future().wait_for(3s) == std::future_status::ready,
        "init handshake");
  Check(!returned, "start waits for init");
  worker.RequestStop();
  release.set_value();
  control.join();
  worker.Join();
  Check(!worker.Post(&EventLoopThreadCompleteLoopProbe),
        "starting stop remembered");
  std::cout << "start_stop: readiness_and_stop_intent=observed\n";
}

void SyscallFailures() {
  auto fds = CountResources("/proc/self/fd");
  create_error = 1;
  ExpectThrows([] {
    EventLoopThread w;
    w.Start();
  });
  register_error = 1;
  ExpectThrows([] {
    EventLoopThread w;
    w.Start();
  });
  for (int fault : {3, 4}) {
    for (bool write : {false, true}) {
      EventLoopThread worker;
      pid_t tid{};

      using RecordWorkerTidTaskTidState = decltype((tid));
      struct RecordWorkerTidTask {
        RecordWorkerTidTaskTidState tid;
        decltype(auto) RecordWorkerTid(EventLoop&) const {
          tid = static_cast<pid_t>(::syscall(SYS_gettid));
        }
      };
      worker.Start(std::bind(&RecordWorkerTidTask::RecordWorkerTid,
                             RecordWorkerTidTask{tid},
                             std::placeholders::_1));
      Blocked(tid);
      if (write)
        write_fault = fault;
      else
        read_fault = fault;
      auto start = std::chrono::steady_clock::now();
      Check(worker.Post(&EventLoopThreadCompleteLoopProbe),
            "wake failure still accepted");
      ExpectThrows([&] { worker.Join(); });
      Check(std::chrono::steady_clock::now() - start < 2s,
            "broken wake bounded");
    }
  }
  Check(CountResources("/proc/self/fd") == fds, "failure rollback fd");
  std::cout << "syscall_failures: create=1 register=1 permanent=2 short=2 "
               "rollback=verified\n";
}

void IoFailureAndFairness() {
  EventLoopThread worker;
  std::unique_ptr<Channel> channel;
  int fd = -1, dispatch_cleanup = 0, cleanup = 0, cancelled = 0;

  struct Capture {
    int& cancelled;
    std::thread::id owner;

    ~Capture() {
      Check(std::this_thread::get_id() == owner, "IO cancel owner");
      ++cancelled;
    }
  };

  struct ChannelCleanupTarget {
    std::unique_ptr<Channel>& channel;
    int& dispatch_cleanup;
    void DrainClosedConnections() {
      channel->Remove();
      ++dispatch_cleanup;
    }
  } cleanup_target{channel, dispatch_cleanup};

  using CleanFailingChannelTaskCleanupState = decltype((cleanup));
  using CleanFailingChannelTaskChannelState = decltype((channel));
  using CleanFailingChannelTaskFdState = decltype((fd));
  struct CleanFailingChannelTask {
    CleanFailingChannelTaskCleanupState cleanup;
    CleanFailingChannelTaskChannelState channel;
    CleanFailingChannelTaskFdState fd;
    decltype(auto) CleanFailingChannel(EventLoop&) const {
      ++cleanup;
      channel->Remove();
      channel.reset();
      ::close(fd);
    }
  };

  using PrepareFailingChannelTaskFdState = decltype((fd));
  using PrepareFailingChannelTaskChannelState = decltype((channel));
  using PrepareFailingChannelTaskCleanupTargetState =
      decltype((cleanup_target));
  using PrepareFailingChannelTaskCancelledState = decltype((cancelled));
  struct PrepareFailingChannelTask {
    PrepareFailingChannelTaskFdState fd;
    PrepareFailingChannelTaskChannelState channel;
    PrepareFailingChannelTaskCleanupTargetState cleanup_target;
    PrepareFailingChannelTaskCancelledState cancelled;
    decltype(auto) PrepareFailingChannel(EventLoop& loop) const {
      fd = ::eventfd(1, EFD_NONBLOCK);
      channel = std::make_unique<Channel>(loop, fd);
      channel->set_HandleConnectionEvent_callback(
          &FailingChannelObserver::HandleConnectionEvent);
      channel->set_interest(EPOLLIN);
      loop.set_DrainClosedConnections_callback(
          std::bind_front(&ChannelCleanupTarget::DrainClosedConnections,
                          &cleanup_target));
      auto capture = std::shared_ptr<Capture>(
          new Capture{cancelled, std::this_thread::get_id()});

      using RetainCancelledPayloadTaskCaptureState =
          std::remove_cvref_t<decltype(capture)>;
      struct RetainCancelledPayloadTask {
        RetainCancelledPayloadTaskCaptureState capture;
        decltype(auto) RetainCancelledPayload() const {}
      };
      loop.QueueInLoop(
          std::bind(&RetainCancelledPayloadTask::RetainCancelledPayload,
                    RetainCancelledPayloadTask{capture}));
    }
  };
  worker.Start(
      std::bind(
          &PrepareFailingChannelTask::PrepareFailingChannel,
          PrepareFailingChannelTask{fd, channel, cleanup_target, cancelled},
          std::placeholders::_1),
      std::bind(&CleanFailingChannelTask::CleanFailingChannel,
                CleanFailingChannelTask{cleanup, channel, fd},
                std::placeholders::_1));
  // Successful init is distinct from a subsequent immediate IO failure.
  ExpectThrows([&] { worker.Join(); });
  Check(dispatch_cleanup == 1 && cleanup == 1 && cancelled == 1,
        "IO exceptional cleanup");
  EventLoop loop;
  fd = ::eventfd(1, EFD_NONBLOCK);
  int io = 0, tasks = 0;
  Channel readable(loop, fd);
  using HandleConnectionEventObserver2State0 = decltype((io));
  struct HandleConnectionEventObserver2 {
    HandleConnectionEventObserver2State0 io;
    void HandleConnectionEvent(std::uint32_t) { ++io; }
  };
  readable.set_HandleConnectionEvent_callback(
      std::bind_front(&HandleConnectionEventObserver2::HandleConnectionEvent,
                      HandleConnectionEventObserver2{io}));
  readable.set_interest(EPOLLIN);
  std::function<void()> chain;

  using RunNextStepTasksState = decltype((tasks));
  using RunNextStepTaskLoopState = decltype((loop));
  using RunNextStepTaskChainState = decltype((chain));
  struct RunNextStepTask {
    RunNextStepTasksState tasks;
    RunNextStepTaskLoopState loop;
    RunNextStepTaskChainState chain;
    decltype(auto) RunNextStep() const {
      if (++tasks < 5) loop.QueueInLoop(chain);
    }
  };
  chain = std::bind(&RunNextStepTask::RunNextStep,
                    RunNextStepTask{tasks, loop, chain});
  loop.QueueInLoop(chain);
  loop.PollOnce(0);
  Check(tasks == 1, "manual snapshot only");
  for (int i = 0; i < 4; ++i) loop.PollOnce(0);
  Check(tasks == 5 && io == 5, "IO advances with nested tasks");
  readable.Remove();
  ::close(fd);
  std::cout << "io_failure: task_accepted=1 executed=0 cancelled=" << cancelled
            << " after_dispatch=" << dispatch_cleanup << " cleanup=" << cleanup
            << " fairness_io=" << io << " tasks=" << tasks << '\n';
}

void SnapshotCancellation() {
  EventLoopThread worker;
  int executed = 0, cancelled = 0;

  struct Capture {
    int& count;
    std::thread::id owner;

    ~Capture() {
      Check(std::this_thread::get_id() == owner, "snapshot cancel owner");
      ++count;
    }
  };

  using QueueCancellationSnapshotTaskExecutedState = decltype((executed));
  using QueueCancellationSnapshotTaskCancelledState = decltype((cancelled));
  struct QueueCancellationSnapshotTask {
    QueueCancellationSnapshotTaskExecutedState executed;
    QueueCancellationSnapshotTaskCancelledState cancelled;
    decltype(auto) QueueCancellationSnapshot(EventLoop& loop) const {
      using FailCancellationSnapshotTaskExecutedState = decltype((executed));
      using FailCancellationSnapshotTaskCancelledState = decltype((cancelled));
      using FailCancellationSnapshotTaskLoopState = decltype((loop));
      struct FailCancellationSnapshotTask {
        FailCancellationSnapshotTaskExecutedState executed;
        FailCancellationSnapshotTaskCancelledState cancelled;
        FailCancellationSnapshotTaskLoopState loop;
        decltype(auto) FailCancellationSnapshot() const {
          ++executed;
          auto nested = std::shared_ptr<Capture>(
              new Capture{cancelled, std::this_thread::get_id()});

          using RetainNestedPayloadTaskNestedState =
              std::remove_cvref_t<decltype(nested)>;
          struct RetainNestedPayloadTask {
            RetainNestedPayloadTaskNestedState nested;
            decltype(auto) RetainNestedPayload() const {}
          };
          loop.QueueInLoop(
              std::bind(&RetainNestedPayloadTask::RetainNestedPayload,
                        RetainNestedPayloadTask{nested}));
          throw std::runtime_error("snapshot");
        }
      };
      loop.QueueInLoop(
          std::bind(&FailCancellationSnapshotTask::FailCancellationSnapshot,
                    FailCancellationSnapshotTask{executed, cancelled, loop}));
      auto capture = std::shared_ptr<Capture>(
          new Capture{cancelled, std::this_thread::get_id()});

      using RetainSnapshotPayloadTaskCaptureState =
          std::remove_cvref_t<decltype(capture)>;
      struct RetainSnapshotPayloadTask {
        RetainSnapshotPayloadTaskCaptureState capture;
        decltype(auto) RetainSnapshotPayload() const {}
      };
      loop.QueueInLoop(
          std::bind(&RetainSnapshotPayloadTask::RetainSnapshotPayload,
                    RetainSnapshotPayloadTask{capture}));
    }
  };
  worker.Start(
      std::bind(&QueueCancellationSnapshotTask::QueueCancellationSnapshot,
                QueueCancellationSnapshotTask{executed, cancelled},
                std::placeholders::_1));
  ExpectThrows([&] { worker.Join(); });
  Check(executed == 1 && cancelled == 2, "snapshot and pending cancelled");
  std::cout << "snapshot_failure: accepted=3 executed=" << executed
            << " cancelled=" << cancelled << '\n';
}

void OwnerAssertions() {
  for (int operation = 0; operation < 3; ++operation) {
    auto child = ::fork();
    Check(child >= 0, "owner probe fork");
    if (!child) {
      rlimit limit{0, 0};
      ::setrlimit(RLIMIT_CORE, &limit);
      auto loop = std::make_unique<EventLoop>();
      int fd = ::eventfd(0, EFD_NONBLOCK);
      auto channel = std::make_unique<Channel>(*loop, fd);
      channel->set_HandleConnectionEvent_callback(
          &PassiveChannelObserver::HandleConnectionEvent);

      using ProbeWrongThreadDestructionTaskOperationState =
          decltype((operation));
      using ProbeWrongThreadDestructionTaskChannelState = decltype((channel));
      using ProbeWrongThreadDestructionTaskLoopState = decltype((loop));
      struct ProbeWrongThreadDestructionTask {
        ProbeWrongThreadDestructionTaskOperationState operation;
        ProbeWrongThreadDestructionTaskChannelState channel;
        ProbeWrongThreadDestructionTaskLoopState loop;
        decltype(auto) ProbeWrongThreadDestruction() const {
          if (operation == 0) channel->Remove();
          if (operation == 1) channel.reset();
          if (operation == 2) loop.reset();
        }
      };
      std::thread wrong(std::bind(
          &ProbeWrongThreadDestructionTask::ProbeWrongThreadDestruction,
          ProbeWrongThreadDestructionTask{operation, channel, loop}));
      wrong.join();
      _exit(1);
    }
    int status{};
    Check(::waitpid(child, &status, 0) == child && WIFSIGNALED(status) &&
              WTERMSIG(status) == SIGABRT,
          "noexcept owner assertion");
  }
  std::cout << "owner_assertions: isolated_SIGABRT=3\n";
}

void Tokens() {
  std::barrier gate(2);
  std::atomic<int> registrations{};

  using RegisterConcurrentTokensTaskGateState = decltype((gate));
  using RegisterConcurrentTokensTaskRegistrationsState =
      decltype((registrations));
  struct RegisterConcurrentTokensTask {
    RegisterConcurrentTokensTaskGateState gate;
    RegisterConcurrentTokensTaskRegistrationsState registrations;
    decltype(auto) RegisterConcurrentTokens() const {
      EventLoop loop;
      int fd = ::eventfd(0, EFD_NONBLOCK);
      Channel channel(loop, fd);
      struct HandleConnectionEventObserver3 {
        void HandleConnectionEvent(std::uint32_t) {}
      };
      channel.set_HandleConnectionEvent_callback(std::bind_front(
          &HandleConnectionEventObserver3::HandleConnectionEvent,
          HandleConnectionEventObserver3{}));
      gate.arrive_and_wait();
      for (int i = 0; i < 1000; ++i) {
        channel.set_interest(EPOLLIN);
        channel.Remove();
        ++registrations;
      }
      ::close(fd);
    }
  };
  auto run = std::bind(&RegisterConcurrentTokensTask::RegisterConcurrentTokens,
                       RegisterConcurrentTokensTask{gate, registrations});
  std::thread a(run), b(run);
  a.join();
  b.join();
  pid_t child = ::fork();
  Check(child >= 0, "fork");
  if (!child) {
    EventLoop loop;
    int fd = ::eventfd(0, EFD_NONBLOCK);
    Channel channel(loop, fd);
    struct HandleConnectionEventObserver4 {
      void HandleConnectionEvent(std::uint32_t) {}
    };
    channel.set_HandleConnectionEvent_callback(
        std::bind_front(&HandleConnectionEventObserver4::HandleConnectionEvent,
                        HandleConnectionEventObserver4{}));
    EventLoopTestAccess::token(std::numeric_limits<std::uint64_t>::max());
    channel.set_interest(EPOLLIN);
    Check(channel.token() == std::numeric_limits<std::uint64_t>::max(),
          "last token");
    channel.Remove();
    ExpectThrows([&] { channel.set_interest(EPOLLIN); });
    ExpectThrows([&] { channel.set_interest(EPOLLIN); });
    ::close(fd);
    _exit(0);
  }
  int status{};
  Check(::waitpid(child, &status, 0) == child && WIFEXITED(status) &&
            WEXITSTATUS(status) == 0,
        "token exhaustion child");
  std::cout << "tokens: concurrent_registrations=" << registrations
            << " exhaustion_latched=2\n";
}
void NamedStartupFirstError() {
  struct StartupTarget {
    int cleanup{0};
    std::thread::id init_owner;
    void Initialize(EventLoop& loop) {
      init_owner = std::this_thread::get_id();
      Check(loop.is_in_loop_thread(), "named init owns loop");
      throw std::runtime_error("named init first");
    }
    void Cleanup(EventLoop& loop) {
      Check(
          loop.is_in_loop_thread() && init_owner == std::this_thread::get_id(),
          "named partial cleanup same owner");
      ++cleanup;
      throw std::runtime_error("named cleanup second");
    }
  } target;
  EventLoopThread worker;
  bool caught = false;
  try {
    worker.Start(std::bind_front(&StartupTarget::Initialize, &target),
                 std::bind_front(&StartupTarget::Cleanup, &target));
  } catch (const std::runtime_error& error) {
    caught = std::string(error.what()) == "named init first";
  }
  worker.Join();
  Check(caught && target.cleanup == 1,
        "thread entry preserves first startup error once");
}

}  // namespace

int main() {
  try {
    OwnerAndReady();
    Producers();
    Wake();
    StopRace();
    NamedStartupFirstError();
    Lifecycle();
    Failures();
    StartStop();
    SyscallFailures();
    IoFailureAndFairness();
    SnapshotCancellation();
    OwnerAssertions();
    Tokens();
    std::cout << "event_loop_thread_tests: PASS\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
