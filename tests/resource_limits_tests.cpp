#include <functional>
#include <type_traits>
#define HP_MULTI_REACTOR_ENTRY legacy_multi_capacity_entry
#include "multi_reactor_tests.cpp"
#undef HP_MULTI_REACTOR_ENTRY

namespace hp::net {
struct ResourceLimitsTestAccess {
  static std::size_t CountResources(EventLoop& loop) {
    std::lock_guard lock(loop.mutex_);
    return loop.outstanding_;
  }
};

struct ConnectionIoTestAccess {
  static std::size_t size(const ConnectionIo& io) {
    return io.output_.readable_bytes();
  }

  static std::size_t capacity(const ConnectionIo& io) {
    return io.output_.capacity();
  }
};
}  // namespace hp::net

namespace {
void ResourceLimitsCompleteQueuedProbe() {}
void ResourceLimitsCompleteLoopProbe(EventLoop&) {}

void OutputBounds() {
  constexpr auto kLimit = ConnectionIo::kOutputLimit;
  Require(ConnectionIo::OutputFits(kLimit - 1, 1) &&
              !ConnectionIo::OutputFits(kLimit, 1) &&
              !ConnectionIo::OutputFits(1, SIZE_MAX),
          "overflow safe capacity arithmetic");
  std::vector<std::byte> bytes(kLimit + 1);
  for (auto size : {kLimit - 1, kLimit, kLimit + 1}) {
    ConnectionIo io{Socket{}};
    bool rejected = false;
    try {
      io.QueueOutput(std::span(bytes).first(size));
    } catch (const std::length_error&) {
      rejected = true;
    }
    Require(rejected == (size > kLimit), "actual output boundary");
    Require(io.pending_bytes() == (rejected ? 0 : size),
            "whole append or no append");
    if (!rejected) {
      try {
        io.QueueOutput(std::span(bytes).first(2));
        throw std::runtime_error("overflow append accepted");
      } catch (const std::length_error&) {
      }
      Require(io.pending_bytes() == size, "rejected append unchanged");
    }
  }
  int sockets[2];
  Require(::socketpair(AF_UNIX,
                       SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                       0,
                       sockets) == 0,
          "bounded output socketpair");
  ConnectionIo io{Socket{sockets[0]}};
  Socket peer{sockets[1]};
  int small = 4096;
  ::setsockopt(io.fd(), SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
  std::size_t total = 0, peak_size = 0, peak_capacity = 0, blocked = 0;
  char received[65536];
  for (int cycle = 0; cycle < 1000; ++cycle) {
    io.QueueOutput(std::span(bytes).first(16384));
    auto written = io.WriteAvailable();
    blocked += written.would_block;
    peak_size = std::max(peak_size, ConnectionIoTestAccess::size(io));
    peak_capacity =
        std::max(peak_capacity, ConnectionIoTestAccess::capacity(io));
    for (;;) {
      const auto n = ::recv(peer.fd(), received, sizeof(received), 0);
      if (n > 0)
        total += static_cast<std::size_t>(n);
      else
        break;
    }
    while (io.pending_bytes() > 16384) {
      (void)io.WriteAvailable();
      const auto n = ::recv(peer.fd(), received, sizeof(received), 0);
      if (n > 0) total += static_cast<std::size_t>(n);
    }
  }
  Require(blocked > 0 && peak_size <= kLimit && peak_capacity <= kLimit * 2,
          "bounded storage");
  std::cout << "output cycles=1000 sent=" << total << " EAGAIN=" << blocked
            << " peak_size=" << peak_size << " peak_capacity=" << peak_capacity
            << '\n';
}

void LoopBounds() {
  EventLoop loop;
  std::atomic<int> executed{0};
  std::vector<int> accepted(2048), calls(2048);
  std::vector<std::thread> producers;
  for (int producer = 0; producer < 4; ++producer) {
    using SubmitCapacityBatchTaskProducerState =
        std::remove_cvref_t<decltype(producer)>;
    using SubmitCapacityBatchTaskAcceptedState = decltype((accepted));
    using SubmitCapacityBatchTaskLoopState = decltype((loop));
    using SubmitCapacityBatchTaskCallsState = decltype((calls));
    using SubmitCapacityBatchTaskExecutedState = decltype((executed));
    struct SubmitCapacityBatchTask {
      SubmitCapacityBatchTaskProducerState producer;
      SubmitCapacityBatchTaskAcceptedState accepted;
      SubmitCapacityBatchTaskLoopState loop;
      SubmitCapacityBatchTaskCallsState calls;
      SubmitCapacityBatchTaskExecutedState executed;
      decltype(auto) SubmitCapacityBatch() const {
        using RecordAcceptedTaskIdState = std::remove_cvref_t<int>;
        using RecordAcceptedTaskCallsState = decltype((calls));
        using RecordAcceptedTaskExecutedState = decltype((executed));
        struct RecordAcceptedTaskTarget {
          RecordAcceptedTaskIdState id;
          RecordAcceptedTaskCallsState calls;
          RecordAcceptedTaskExecutedState executed;
          decltype(auto) RecordAcceptedTask() const {
            ++calls[id];
            ++executed;
          }
        };
        for (int id = producer * 512; id < (producer + 1) * 512; ++id)
          accepted[id] = loop.QueueInLoop(
              std::bind(&RecordAcceptedTaskTarget::RecordAcceptedTask,
                        RecordAcceptedTaskTarget{id, calls, executed}));
      }
    };
    producers.emplace_back(std::bind(
        &SubmitCapacityBatchTask::SubmitCapacityBatch,
        SubmitCapacityBatchTask{producer, accepted, loop, calls, executed}));
  }
  for (auto& producer : producers) producer.join();
  Require(std::count(accepted.begin(), accepted.end(), 1) == 1024,
          "multi producer capacity");
  loop.PollOnce(0);
  Require(accepted == calls && executed == 1024, "each accepted ID once");

  using RecordRestoredCapacityTaskExecutedState = decltype((executed));
  struct RecordRestoredCapacityTask {
    RecordRestoredCapacityTaskExecutedState executed;
    decltype(auto) RecordRestoredCapacity() const { ++executed; }
  };
  Require(loop.QueueInLoop(
              std::bind(&RecordRestoredCapacityTask::RecordRestoredCapacity,
                        RecordRestoredCapacityTask{executed})),
          "capacity restored");
  loop.PollOnce(0);
  std::cout << "loop accepted=1024 rejected=1024 executed=" << executed << '\n';
}

void ReleaseAndFaults() {
  EventLoop loop;
  int accepted_count = 0, executed_count = 0;
  bool injected = false;
  for (int index = 0; index < 100 && !injected; ++index) {
    timer_allocation_failure = 0;
    try {
      using RecordAllocationProbeTaskExecutedCountState =
          decltype((executed_count));
      struct RecordAllocationProbeTask {
        RecordAllocationProbeTaskExecutedCountState executed_count;
        decltype(auto) RecordAllocationProbe() const { ++executed_count; }
      };
      Require(loop.QueueInLoop(
                  std::bind(&RecordAllocationProbeTask::RecordAllocationProbe,
                            RecordAllocationProbeTask{executed_count})),
              "allocation probe admitted");
      ++accepted_count;
    } catch (const std::bad_alloc&) {
      injected = true;
    }
    timer_allocation_failure = -1;
  }
  Require(injected && ResourceLimitsTestAccess::CountResources(loop) ==
                          static_cast<std::size_t>(accepted_count),
          "enqueue allocation failure returns reservation");

  using RecordRestoredQuotaTaskExecutedCountState = decltype((executed_count));
  struct RecordRestoredQuotaTask {
    RecordRestoredQuotaTaskExecutedCountState executed_count;
    decltype(auto) RecordRestoredQuota() const { ++executed_count; }
  };
  Require(
      loop.QueueInLoop(std::bind(&RecordRestoredQuotaTask::RecordRestoredQuota,
                                 RecordRestoredQuotaTask{executed_count})),
      "enqueue after allocation failure");
  ++accepted_count;
  loop.PollOnce(0);
  Require(executed_count == accepted_count &&
              ResourceLimitsTestAccess::CountResources(loop) == 0,
          "all reservations released");
  int released = 0;

  struct Capture {
    EventLoop& loop;
    int& released;

    ~Capture() {
      ++released;
      Require(!loop.QueueInLoop(&ResourceLimitsCompleteQueuedProbe),
              "terminal destructor reentry rejected");
      loop.RequestForce();
    }
  };

  auto capture = std::shared_ptr<Capture>(new Capture{loop, released});

  struct ThrowCapacityFailureTask {
    decltype(auto) ThrowCapacityFailure() const {
      throw std::runtime_error("capacity original failure");
    }
  };
  loop.QueueInLoop(std::bind(&ThrowCapacityFailureTask::ThrowCapacityFailure,
                             ThrowCapacityFailureTask{}));

  using RetainCancellationPayloadTaskCaptureState =
      std::remove_cvref_t<decltype(capture)>;
  struct RetainCancellationPayloadTask {
    RetainCancellationPayloadTaskCaptureState capture;
    decltype(auto) RetainCancellationPayload() const {}
  };
  loop.QueueInLoop(
      std::bind(&RetainCancellationPayloadTask::RetainCancellationPayload,
                RetainCancellationPayloadTask{capture}));
  capture.reset();
  try {
    loop.PollOnce(0);
    throw std::runtime_error("missing capacity failure");
  } catch (const std::runtime_error& error) {
    Require(std::string(error.what()) == "capacity original failure",
            "original task failure");
  }
  Require(released == 1 && ResourceLimitsTestAccess::CountResources(loop) == 0,
          "fatal batch count released");
  std::cout << "task_fault accepted=" << accepted_count
            << " executed=" << executed_count << " capture_release=" << released
            << " outstanding=" << ResourceLimitsTestAccess::CountResources(loop)
            << '\n';
}

void SaturatedControls() {
  EventLoop loop;
  int drain = 0, force = 0, ran = 0;
  struct SaturatedControlTarget {
    EventLoop& loop;
    int& drain;
    int& force;
    void HandleControl(EventLoop::Control control, EventLoop::Deadline) {
      Require(loop.is_in_loop_thread(), "control owner");
      if (control == EventLoop::Control::kDrain) ++drain;
      if (control == EventLoop::Control::kForce) ++force;
    }
  } control_target{loop, drain, force};
  loop.set_HandleControl_callback(
      std::bind_front(&SaturatedControlTarget::HandleControl, &control_target));

  using VerifyControlBeforeTaskIdState = std::remove_cvref_t<int>;
  using VerifyControlBeforeTaskDrainState = decltype((drain));
  using VerifyControlBeforeTaskLoopState = decltype((loop));
  using VerifyControlBeforeTaskForceState = decltype((force));
  using VerifyControlBeforeTaskRanState = decltype((ran));
  struct VerifyControlBeforeTaskTarget {
    VerifyControlBeforeTaskIdState id;
    VerifyControlBeforeTaskDrainState drain;
    VerifyControlBeforeTaskLoopState loop;
    VerifyControlBeforeTaskForceState force;
    VerifyControlBeforeTaskRanState ran;
    decltype(auto) VerifyControlBeforeTask() const {
      Require(drain == 1, "drain before first ordinary callback");
      if (id == 0)
        loop.RequestForce();
      else
        Require(force == 1, "force at callback boundary");
      ++ran;
    }
  };
  for (int id = 0; id < 1024; ++id)
    Require(loop.QueueInLoop(std::bind(
                &VerifyControlBeforeTaskTarget::VerifyControlBeforeTask,
                VerifyControlBeforeTaskTarget{id, drain, loop, force, ran})),
            "control full queue");
  Require(!loop.QueueInLoop(&ResourceLimitsCompleteQueuedProbe),
          "ordinary queue full");
  loop.RequestDrain(hp::timer::TimerQueue::Clock::now() +
                    std::chrono::hours(1));
  loop.PollOnce(0);
  Require(drain == 1 && force == 1 && ran == 1024,
          "fixed control bypass and accepted drain");
  std::cout << "saturated_control drain=" << drain << " force=" << force
            << " tasks=" << ran << '\n';
}

void OversizedConnection() {
  EventLoop loop;
  ConnectionRegistry registry(loop, 0);
  int first[2], second[2];
  Require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, first) == 0,
          "overflow pair");
  Require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, second) == 0,
          "healthy pair");
  Socket peer1(first[1]), peer2(second[1]);
  {
    std::lock_guard lock(socket_probe_mutex);
    accepted_fds.insert(first[0]);
    accepted_fds.insert(second[0]);
    accepted_sockets += 2;
  }
  std::vector<std::byte> excessive(ConnectionIo::kOutputLimit + 1);
  struct OversizedOutputHandler {
    std::vector<std::byte>& excessive;
    void HandleMessage(TcpConnection& connection,
                       std::span<const std::byte>,
                       bool) {
      connection.Send(excessive);
    }
  };
  registry.AddConnection(Socket(first[0]),
                         std::bind_front(&OversizedOutputHandler::HandleMessage,
                                         OversizedOutputHandler{excessive}));
  registry.AddConnection(Socket(second[0]), {});
  Require(::send(peer1.fd(), "x", 1, MSG_NOSIGNAL) == 1, "trigger overflow");
  Require(::send(peer2.fd(), "ok", 2, MSG_NOSIGNAL) == 2, "healthy request");
  loop.PollOnce(0);
  char bytes[4];
  Require(::recv(peer1.fd(), bytes, sizeof(bytes), 0) == 0,
          "overflow has no error response");
  Require(::recv(peer2.fd(), bytes, sizeof(bytes), 0) == 2 &&
              std::string_view(bytes, 2) == "ok",
          "healthy connection unaffected");
  Require(!loop.failed(), "oversized output is not worker fatal");
  std::cout << "oversized_connection rejected_bytes=" << excessive.size()
            << " healthy_bytes=2\n";
}

void BatchAndPool() {
  EventLoopThread thread;
  std::promise<void> entered, release;
  auto gate = release.get_future().share();
  std::atomic<int> completed{0};

  using FillWorkerQueueTaskEnteredState = decltype((entered));
  using FillWorkerQueueTaskGateState = decltype((gate));
  using FillWorkerQueueTaskCompletedState = decltype((completed));
  struct FillWorkerQueueTask {
    FillWorkerQueueTaskEnteredState entered;
    FillWorkerQueueTaskGateState gate;
    FillWorkerQueueTaskCompletedState completed;
    decltype(auto) FillWorkerQueue(EventLoop& loop) const {
      using WaitForWorkerQueueReleaseTaskIdState = std::remove_cvref_t<int>;
      using WaitForWorkerQueueReleaseTaskEnteredState = decltype((entered));
      using WaitForWorkerQueueReleaseTaskGateState = decltype((gate));
      using WaitForWorkerQueueReleaseTaskCompletedState = decltype((completed));
      using WaitForWorkerQueueReleaseTaskOwnerState = decltype(&loop);
      struct WaitForWorkerQueueReleaseTask {
        WaitForWorkerQueueReleaseTaskIdState id;
        WaitForWorkerQueueReleaseTaskEnteredState entered;
        WaitForWorkerQueueReleaseTaskGateState gate;
        WaitForWorkerQueueReleaseTaskCompletedState completed;
        WaitForWorkerQueueReleaseTaskOwnerState owner;
        decltype(auto) WaitForWorkerQueueRelease() const {
          if (id == 0) {
            Require(!owner->QueueInLoop(&ResourceLimitsCompleteQueuedProbe),
                    "executing task counts on self post");
            entered.set_value();
            Require(gate.wait_for(3s) == std::future_status::ready,
                    "batch gate");
          }
          ++completed;
        }
      };
      for (int id = 0; id < 1024; ++id)
        Require(loop.QueueInLoop(std::bind(
                    &WaitForWorkerQueueReleaseTask::WaitForWorkerQueueRelease,
                    WaitForWorkerQueueReleaseTask{id,
                                                  entered,
                                                  gate,
                                                  completed,
                                                  &loop})),
                "ready task accepted");
    }
  };
  thread.Start(std::bind(&FillWorkerQueueTask::FillWorkerQueue,
                         FillWorkerQueueTask{entered, gate, completed},
                         std::placeholders::_1));
  Require(entered.get_future().wait_for(3s) == std::future_status::ready,
          "batch entered");
  Require(!thread.Post(&ResourceLimitsCompleteLoopProbe),
          "thread rejects when local batch full");
  release.set_value();
  thread.RequestStop();
  thread.Join();
  Require(completed == 1024, "S1 accepted drain preserved");
  EventLoopThreadPool pool;
  std::promise<void> pool_entered, pool_release;
  auto pool_gate = pool_release.get_future().share();

  using FillPoolQueueTaskPoolEnteredState = decltype((pool_entered));
  using FillPoolQueueTaskPoolGateState = decltype((pool_gate));
  struct FillPoolQueueTask {
    FillPoolQueueTaskPoolEnteredState pool_entered;
    FillPoolQueueTaskPoolGateState pool_gate;
    decltype(auto) FillPoolQueue(std::size_t, EventLoop& loop) const {
      using WaitForPoolQueueReleaseTaskIdState = std::remove_cvref_t<int>;
      using WaitForPoolQueueReleaseTaskPoolEnteredState =
          decltype((pool_entered));
      using WaitForPoolQueueReleaseTaskPoolGateState = decltype((pool_gate));
      struct WaitForPoolQueueReleaseTask {
        WaitForPoolQueueReleaseTaskIdState id;
        WaitForPoolQueueReleaseTaskPoolEnteredState pool_entered;
        WaitForPoolQueueReleaseTaskPoolGateState pool_gate;
        decltype(auto) WaitForPoolQueueRelease() const {
          if (id == 0) {
            pool_entered.set_value();
            Require(pool_gate.wait_for(3s) == std::future_status::ready,
                    "pool gate");
          }
        }
      };
      for (int id = 0; id < 1024; ++id)
        Require(loop.QueueInLoop(std::bind(
                    &WaitForPoolQueueReleaseTask::WaitForPoolQueueRelease,
                    WaitForPoolQueueReleaseTask{id, pool_entered, pool_gate})),
                "fill loop bypassing pool");
    }
  };
  pool.Start(1,
             std::bind(&FillPoolQueueTask::FillPoolQueue,
                       FillPoolQueueTask{pool_entered, pool_gate},
                       std::placeholders::_1,
                       std::placeholders::_2));
  Require(pool_entered.get_future().wait_for(3s) == std::future_status::ready,
          "pool entered");
  Require(!pool.Post(0, &ResourceLimitsCompleteLoopProbe),
          "pool rollback after loop rejection");
  Require(EventLoopThreadPoolTestAccess::outstanding(pool, 0) == 0,
          "double reservation restored");
  pool_release.set_value();
  pool.RequestStop();
  pool.Join();
  std::cout << "batch executed=" << completed
            << " pool_rejected_outstanding=0\n";
}
void DestructionReservations() {
  EventLoop loop;
  int releases = 0;
  std::size_t loop_destruction_count = 0;
  bool loop_destruction_owner = false;
  struct LoopCapture {
    EventLoop& loop;
    int& releases;
    std::size_t& destruction_count;
    bool& destruction_owner;
    ~LoopCapture() {
      destruction_count = ResourceLimitsTestAccess::CountResources(loop);
      destruction_owner = loop.is_in_loop_thread();
      ++releases;
    }
  };
  auto capture =
      std::shared_ptr<LoopCapture>(new LoopCapture{loop,
                                                   releases,
                                                   loop_destruction_count,
                                                   loop_destruction_owner});
  struct LoopTaskTarget {
    std::shared_ptr<LoopCapture> capture;
    void RetainLoopReservation() const {}
  };
  EventLoop::LoopTask task =
      std::bind_front(&LoopTaskTarget::RetainLoopReservation,
                      LoopTaskTarget{capture});
  capture.reset();
  Require(loop.QueueInLoop(std::move(task)), "destruction quota task admitted");
  loop.PollOnce(0);
  Require(loop_destruction_count == 1,
          "capacity includes user capture destruction");
  Require(loop_destruction_owner, "capture destruction on loop owner");
  Require(releases == 1 && ResourceLimitsTestAccess::CountResources(loop) == 0,
          "destruction returns reservation once");

  EventLoopThreadPool pool;
  std::thread::id owner;
  struct WorkerOwnerTarget {
    std::thread::id& owner;
    void RecordWorkerOwner(std::size_t, EventLoop&) const {
      owner = std::this_thread::get_id();
    }
  };
  pool.Start(1,
             std::bind_front(&WorkerOwnerTarget::RecordWorkerOwner,
                             WorkerOwnerTarget{owner}));
  struct PoolCapture {
    EventLoopThreadPool& pool;
    std::thread::id& owner;
    int& releases;
    ~PoolCapture() {
      Require(EventLoopThreadPoolTestAccess::outstanding(pool, 0) == 1,
              "pool ticket retained during user capture destruction");
      Require(owner == std::this_thread::get_id(),
              "pool capture release owner");
      ++releases;
    }
  };
  auto pooled =
      std::shared_ptr<PoolCapture>(new PoolCapture{pool, owner, releases});
  struct PoolTaskTarget {
    std::shared_ptr<PoolCapture> capture;
    void RetainPoolReservation(EventLoop&) const {}
  };
  EventLoopThread::LoopTask posted =
      std::bind_front(&PoolTaskTarget::RetainPoolReservation,
                      PoolTaskTarget{pooled});
  pooled.reset();
  Require(pool.Post(0, std::move(posted)), "pool destruction task admitted");
  pool.RequestStop();
  pool.Join();
  Require(
      releases == 2 && EventLoopThreadPoolTestAccess::outstanding(pool, 0) == 0,
      "pool capture then ticket released exactly once");
  std::cout << "destruction_quota loop=1 pool=1 final=0 owner=verified\n";
}

void PostWrapperAllocationFailure() {
  EventLoopThread worker;
  worker.Start();
  int releases = 0;
  const auto producer = std::this_thread::get_id();
  struct Capture {
    EventLoopThread& worker;
    int& releases;
    std::thread::id producer;
    ~Capture() {
      Require(std::this_thread::get_id() == producer,
              "rejected wrapper payload released on producer");
      worker.RequestStop();  // Reenters the forwarding mutex; must be unlocked.
      ++releases;
    }
  };
  auto capture =
      std::shared_ptr<Capture>(new Capture{worker, releases, producer});
  struct RejectedTaskTarget {
    std::shared_ptr<Capture> capture;
    void RejectUnexpectedExecution(EventLoop&) const {
      throw std::runtime_error("rejected wrapper executed");
    }
  };
  EventLoopThread::LoopTask task =
      std::bind_front(&RejectedTaskTarget::RejectUnexpectedExecution,
                      RejectedTaskTarget{capture});
  capture.reset();
  bool rejected = false;
  timer_allocation_failure =
      0;  // Payload already built: fail wrapper allocation.
  try {
    (void)worker.Post(std::move(task));
  } catch (const std::bad_alloc&) {
    rejected = true;
  }
  timer_allocation_failure = -1;
  worker.Join();
  Require(rejected && releases == 1,
          "wrapper allocation failure releases payload once outside forwarding "
          "lock");
  std::cout << "post_wrapper injected=1 accepted=0 producer_release=1 "
               "reentry=verified\n";
}

}  // namespace

int main() {
  try {
    DestructionReservations();
    PostWrapperAllocationFailure();
    OutputBounds();
    LoopBounds();
    BatchAndPool();
    ReleaseAndFaults();
    SaturatedControls();
    OversizedConnection();
    Require(accepted_sockets == closed_sockets && invalid_closes == 0,
            "observed fd closes once");
    std::cout << "socket_closes accepted=" << accepted_sockets
              << " closed=" << closed_sockets << " invalid=" << invalid_closes
              << '\n';
    std::cout << "resource_limits_tests: PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
