#include <functional>
#include <type_traits>
// Reuse the original production fixture and close probes without changing its
// assertions.
#include <climits>
#define HP_MULTI_REACTOR_ENTRY legacy_multi_reactor_entry
#include "multi_reactor_tests.cpp"
#undef HP_MULTI_REACTOR_ENTRY
#define main legacy_server_entry
#include "../app/main.cpp"
#undef main

namespace hp::net {
struct ConnectionTimeoutTestAccess {
  static ConnectionRegistry& registry(TcpServer& server, std::size_t index) {
    return server.worker_count_ ? *server.registries_.at(index)
                                : *server.main_registry_;
  }

  static EventLoop& loop(ConnectionRegistry& registry) {
    return registry.loop_;
  }

  static auto& entries(ConnectionRegistry& registry) {
    return registry.connections_;
  }

  static auto deadline(EventLoop& loop) { return loop.timers_.next_deadline(); }

  static auto progress(TcpConnection& connection) {
    return connection.last_progress_;
  }

  static auto wait_since(TcpConnection& connection) {
    return connection.wait_since_;
  }

  static auto timer(TcpConnection& connection) {
    return connection.timeout_id_;
  }

  static void Event(TcpConnection& connection, std::uint32_t event) {
    connection.HandleConnectionEvent(event);
  }

  static void Expire(ConnectionRegistry& registry,
                     int fd,
                     TcpConnection::Identity identity) {
    registry.ExpireConnection(fd, identity);
  }

  static bool stopping(TcpServer& server) { return server.stopping_.load(); }

  static auto config(TcpServer& server) { return server.timeouts_; }
};
}  // namespace hp::net

namespace {
void ConnectionTimeoutCompleteLoopProbe(EventLoop&) {}
void ConnectionTimeoutCompleteQueuedProbe() {}

using Access = ConnectionTimeoutTestAccess;
using Clock = hp::timer::TimerQueue::Clock;

long long Ticks(Clock::time_point time) {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             time.time_since_epoch())
      .count();
}

struct Snapshot {
  std::size_t connections{}, timers{}, waiting{}, pending{};
  long long progress{}, deadline{}, wait{};
};

Snapshot CollectTimeoutSnapshot(ServerHarness& harness,
                                std::size_t workers,
                                std::size_t index = 0) {
  std::promise<Snapshot> done;
  auto result = done.get_future();

  using CollectConnectionSnapshotTaskHarnessState = decltype((harness));
  using CollectConnectionSnapshotTaskIndexState = decltype((index));
  using CollectConnectionSnapshotTaskDoneState = decltype((done));
  struct CollectConnectionSnapshotTask {
    CollectConnectionSnapshotTaskHarnessState harness;
    CollectConnectionSnapshotTaskIndexState index;
    CollectConnectionSnapshotTaskDoneState done;
    decltype(auto) CollectConnectionSnapshot(EventLoop& loop) const {
      Snapshot value;
      auto& registry = Access::registry(*harness.server, index);
      value.connections = Access::entries(registry).size();
      value.timers = loop.timer_count();
      for (auto& [fd, connection] : Access::entries(registry)) {
        (void)fd;
        value.pending += connection->pending_bytes();
        value.waiting += Access::wait_since(*connection).has_value();
        value.progress = Ticks(Access::progress(*connection));
        if (auto wait = Access::wait_since(*connection))
          value.wait = Ticks(*wait);
      }
      if (auto deadline = Access::deadline(loop))
        value.deadline = Ticks(*deadline);
      done.set_value(value);
    }
  };
  auto inspect =
      std::bind(&CollectConnectionSnapshotTask::CollectConnectionSnapshot,
                CollectConnectionSnapshotTask{harness, index, done},
                std::placeholders::_1);
  bool accepted;
  if (workers)
    accepted = TcpServerTestAccess::Post(*harness.server, index, inspect);
  else
    accepted = TcpServerTestAccess::MainPost(
        *harness.server,
        std::bind_front(
            inspect,
            std::ref(Access::loop(Access::registry(*harness.server, 0)))));
  Require(accepted, "snapshot task accepted");
  Require(result.wait_for(3s) == std::future_status::ready,
          "snapshot deadline");
  return result.get();
}

void IdleModes(const hp::http::StaticFileService& service) {
  for (std::size_t workers : {0U, 1U, 2U}) {
    ServerHarness harness(service, workers, nullptr, {150ms, 0ms});
    Stream client(harness.port);
    Snapshot state;
    WaitUntil([&] {
      state = CollectTimeoutSnapshot(harness, workers);
      return state.connections == 1;
    });
    Require(state.timers == 1 && state.deadline - state.progress == 150000,
            "adopt starts exactly one idle timer");
    client.ExpectEof();
    const auto closed = Ticks(Clock::now());
    auto after = CollectTimeoutSnapshot(harness, workers);
    Require(after.connections == 0 && after.timers == 0,
            "pure timer immediately reclaimed");
    Require(closed >= state.deadline, "idle not early");
    harness.Stop();
    std::cout << "idle workers=" << workers << " adopted_us=" << state.progress
              << " deadline_us=" << state.deadline << " closed_us=" << closed
              << " timers=" << state.timers << "->" << after.timers << '\n';
  }
}

void WaitingStates(const hp::http::StaticFileService& service) {
  for (const auto config : {ConnectionTimeouts{0ms, 120ms},
                            ConnectionTimeouts{200ms, 0ms},
                            ConnectionTimeouts{200ms, 120ms},
                            ConnectionTimeouts{0ms, 0ms}}) {
    ServerHarness harness(service, 2, nullptr, config);
    Stream client(harness.port);
    Snapshot initial;
    WaitUntil([&] {
      initial = CollectTimeoutSnapshot(harness, 2);
      return initial.connections == 1;
    });
    Require(initial.waiting == 0 &&
                initial.timers == (config.idle.count() ? 1U : 0U),
            "initial connection is not keep-alive waiting");
    client.Send(Query("/note.txt") + Query("/missing"));
    Response(client.ReadResponse(), 200, "hello from S3\n");
    Response(client.ReadResponse(), 404, "404 Not Found\n");
    auto state = CollectTimeoutSnapshot(harness, 2);
    Require(state.waiting == 1 && state.pending == 0,
            "pipeline drained before wait");
    Require(state.timers ==
                (config.idle.count() || config.keep_alive.count() ? 1U : 0U),
            "disabled policies allocate no timers");
    auto expected = config.idle.count()
                        ? state.progress + config.idle.count() * 1000
                        : LLONG_MAX;
    if (config.keep_alive.count())
      expected =
          std::min(expected, state.wait + config.keep_alive.count() * 1000);
    if (state.timers)
      Require(state.deadline == expected, "minimum applicable deadline");
    client.Send("GET /note.txt HTTP/1.1\r\nHost:");
    Snapshot partial;
    WaitUntil([&] {
      partial = CollectTimeoutSnapshot(harness, 2);
      return partial.waiting == 0;
    });
    Require(partial.progress > state.progress,
            "new bytes refresh idle and exit waiting");
    Require(partial.timers == (config.idle.count() ? 1U : 0U),
            "partial has no keep timer");
    client.Send(" localhost\r\n\r\n");
    Response(client.ReadResponse(), 200, "hello from S3\n");
    state = CollectTimeoutSnapshot(harness, 2);
    Require(state.waiting == 1, "second response enters wait");
    if (state.timers) {
      client.ExpectEof();
      Require(Ticks(Clock::now()) >= state.deadline,
              "combined deadline not early");
    } else {
      harness.server->RequestStop();
    }
    harness.Stop();
    std::cout << "waiting idle_ms=" << config.idle.count()
              << " keep_ms=" << config.keep_alive.count()
              << " wait_us=" << state.wait << " deadline_us=" << state.deadline
              << " partial_timers=" << partial.timers
              << " pipeline_responses=2\n";
  }
}

struct Pair {
  Socket peer;
  int owned;

  Pair() {
    int sockets[2];
    Require(::socketpair(AF_UNIX,
                         SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                         0,
                         sockets) == 0,
            "socketpair");
    owned = sockets[0];
    peer.Reset(sockets[1]);
    std::lock_guard lock(socket_probe_mutex);
    accepted_fds.insert(owned);
    ++accepted_sockets;
  }
};

void OwnerProgressAndFailures(const hp::http::StaticFileService& service) {
  EventLoop loop;
  ConnectionRegistry registry(loop, hp::http::kMaxRequestBytes, {150ms, 100ms});
  Pair pair;
  registry.AddConnection(Socket(pair.owned),
                         hp::app::MakeHttpFactory(service)());
  auto& connection = *Access::entries(registry).at(pair.owned);
  const auto initial = Access::progress(connection);
  const auto id = Access::timer(connection);
  Access::Event(connection, EPOLLIN | EPOLLOUT);
  Require(Access::progress(connection) == initial &&
              Access::timer(connection) == id,
          "EAGAIN and spurious writable do not refresh");
  connection.set_idle_wait(true);
  auto wait = Access::wait_since(connection);
  connection.set_idle_wait(true);
  Require(Access::wait_since(connection) == wait,
          "duplicate wait does not renew");
  connection.set_idle_wait(false);
  const std::string partial = "GET /note.txt HTTP/1.1\r\nHost:";
  Require(
      ::send(pair.peer.fd(), partial.data(), partial.size(), MSG_NOSIGNAL) ==
          static_cast<ssize_t>(partial.size()),
      "partial send bytes");
  loop.PollOnce(0);
  Require(
      Access::progress(connection) > initial && !Access::wait_since(connection),
      "actual recv refresh");
  const auto before_failure = Access::progress(connection);
  timer_allocation_failure = 0;
  try {
    connection.set_idle_wait(true);
    throw std::runtime_error("renewal failure not raised");
  } catch (const std::bad_alloc&) {
  }
  timer_allocation_failure = -1;
  Require(connection.state() == TcpConnection::State::kClosing &&
              loop.timer_count() == 0,
          "renew failure closes connection and cancels record");
  registry.DrainClosedConnections();
  Require(Access::entries(registry).empty(), "failed renewal reclaimed");
  Require(!loop.CancelTimer(id), "old timer id invalid");
  std::cout << "progress recv_bytes=" << partial.size()
            << " adopted_us=" << Ticks(initial)
            << " recv_us=" << Ticks(before_failure)
            << " EAGAIN_delta=0 renewal_failure_timers=" << loop.timer_count()
            << '\n';
}

void WriteProgress(const hp::http::StaticFileService& service) {
  EventLoop loop;
  ConnectionRegistry registry(loop, hp::http::kMaxRequestBytes, {150ms, 100ms});
  Pair pair;
  int small = 4096;
  ::setsockopt(pair.owned, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
  int provider_calls = 0;
  using ResponseScenario1State0 = decltype((provider_calls));
  using ResponseScenario1State1 = decltype((service));
  struct ResponseScenario1 {
    ResponseScenario1State0 provider_calls;
    ResponseScenario1State1 service;
    hp::http::ResponseResult PrepareResponse(
        const hp::http::HttpRequest& request,
        hp::http::ConnectionPolicy policy) {
      ++provider_calls;
      return service.HandleResponse(request, policy);
    }
  };
  registry.AddConnection(Socket(pair.owned),
                         hp::app::MakeHttpCallback(std::bind_front(
                             &ResponseScenario1::PrepareResponse,
                             ResponseScenario1{provider_calls, service})));
  auto& connection = *Access::entries(registry).at(pair.owned);
  const std::string request = Query("/large.bin") + Query("/note.txt");
  Require(
      ::send(pair.peer.fd(), request.data(), request.size(), MSG_NOSIGNAL) ==
          static_cast<ssize_t>(request.size()),
      "large request");
  loop.PollOnce(0);
  Require(connection.pending_bytes() > 0 && !Access::wait_since(connection),
          "Writing never waits");
  auto progress = Access::progress(connection);
  const auto queued = connection.pending_bytes();
  Access::Event(connection, EPOLLOUT);
  Require(Access::progress(connection) == progress &&
              connection.pending_bytes() == queued,
          "real send EAGAIN does not refresh");
  char bytes[65536];
  std::size_t received = 0;
  const auto began = Clock::now();
  for (int cycle = 0; cycle < 12; ++cycle) {
    auto n = ::recv(pair.peer.fd(), bytes, sizeof(bytes), 0);
    Require(n > 0, "drain real response bytes");
    received += static_cast<std::size_t>(n);
    loop.PollOnce(0);
    bool paced = false;

    using RecordPacingDeadlineTaskPacedState = decltype((paced));
    struct RecordPacingDeadlineTask {
      RecordPacingDeadlineTaskPacedState paced;
      decltype(auto) RecordPacingDeadline() const { paced = true; }
    };
    loop.AddTimer(Clock::now() + 30ms,
                  std::bind(&RecordPacingDeadlineTask::RecordPacingDeadline,
                            RecordPacingDeadlineTask{paced}));
    while (!paced) loop.PollOnce(-1);
  }
  Require(Clock::now() - began > 150ms,
          "write progress survives original idle deadline");
  Require(Access::progress(connection) > progress &&
              connection.pending_bytes() < queued,
          "actual send progress renews idle");
  progress = Access::progress(connection);
  const auto pending = connection.pending_bytes();
  while (!Access::entries(registry).empty()) loop.PollOnce(-1);
  Require(Ticks(Clock::now()) >= Ticks(progress) + 150000,
          "stalled writer deadline");
  Require(loop.timer_count() == 0, "stalled writer timer reclaimed");
  Require(provider_calls == 1,
          "buffered suffix provider not called after timeout");
  std::string tail;
  for (;;) {
    auto n = ::recv(pair.peer.fd(), bytes, sizeof(bytes), 0);
    if (n <= 0) break;
    tail.append(bytes, static_cast<std::size_t>(n));
  }
  Require(tail.find("408") == std::string::npos,
          "no synthetic timeout response");
  std::cout << "write received_bytes=" << received
            << " progress_duration_us=" << Ticks(Clock::now()) - Ticks(began)
            << " initial_pending=" << queued << " stalled_pending=" << pending
            << " last_send_us=" << Ticks(progress)
            << " closed_us=" << Ticks(Clock::now())
            << " provider_calls=" << provider_calls
            << " timers=" << loop.timer_count() << '\n';
}

void PacedInput(const hp::http::StaticFileService& service) {
  EventLoop loop;
  ConnectionRegistry registry(loop, hp::http::kMaxRequestBytes, {150ms, 100ms});
  Pair pair;
  registry.AddConnection(Socket(pair.owned),
                         hp::app::MakeHttpFactory(service)());
  const auto began = Clock::now();
  std::size_t sent = 0;
  for (const std::string_view part :
       {"GET ", "/note.txt ", "HTTP/1.1\r\n", "Host: localhost\r\n", "\r\n"}) {
    Require(::send(pair.peer.fd(), part.data(), part.size(), MSG_NOSIGNAL) ==
                static_cast<ssize_t>(part.size()),
            "paced request send");
    sent += part.size();
    loop.PollOnce(0);
    bool paced = false;

    using RecordPacingDeadlineTaskPacedState = decltype((paced));
    struct RecordPacingDeadlineTask {
      RecordPacingDeadlineTaskPacedState paced;
      decltype(auto) RecordPacingDeadline() const { paced = true; }
    };
    loop.AddTimer(Clock::now() + 50ms,
                  std::bind(&RecordPacingDeadlineTask::RecordPacingDeadline,
                            RecordPacingDeadlineTask{paced}));
    while (!paced) loop.PollOnce(-1);
    Require(Access::entries(registry).size() == 1,
            "positive chunks keep connection alive");
  }
  Require(Clock::now() - began > 150ms, "input spans original idle cutoff");
  char bytes[1024];
  const auto count = ::recv(pair.peer.fd(), bytes, sizeof(bytes), 0);
  Require(count > 0, "paced request received response");
  const std::string response_bytes(bytes, static_cast<std::size_t>(count));
  Require(response_bytes.starts_with("HTTP/1.1 200") &&
              response_bytes.ends_with("hello from S3\n"),
          "paced complete response");
  std::cout << "paced_input sent_bytes=" << sent << " response_bytes=" << count
            << " elapsed_us=" << Ticks(Clock::now()) - Ticks(began)
            << " timers=" << loop.timer_count() << '\n';
}

void HealthyDuringTimeout(const hp::http::StaticFileService& service) {
  ServerHarness harness(service, 2, nullptr, {150ms, 100ms});
  Stream stalled(harness.port);
  stalled.Send(Query("/large.bin") + Query("/note.txt"));
  Snapshot blocked;
  WaitUntil([&] {
    blocked = CollectTimeoutSnapshot(harness, 2, 0);
    return blocked.pending > 0;
  });
  Require(blocked.waiting == 0, "production stalled writer is not waiting");
  Stream healthy(harness.port);
  healthy.Send(Query("/note.txt", true));
  Response(healthy.ReadResponse(), 200, "hello from S3\n");
  healthy.ExpectEof();
  Snapshot after;
  WaitUntil([&] {
    after = CollectTimeoutSnapshot(harness, 2, 0);
    return after.connections == 0;
  });
  const auto closed = Ticks(Clock::now());
  Require(after.timers == 0 && closed >= blocked.deadline,
          "production writer expiry reclaimed");
  harness.Stop();
  std::cout << "healthy_during_timeout pending_bytes=" << blocked.pending
            << " deadline_us=" << blocked.deadline << " closed_us=" << closed
            << " healthy_responses=1 timers_after=" << after.timers << '\n';
}

void RollbackAndReuse(const hp::http::StaticFileService& service) {
  EventLoop loop;
  ConnectionRegistry registry(loop, hp::http::kMaxRequestBytes, {150ms, 100ms});
  int failures_seen = 0;
  for (int allocation = 0; allocation < 4; ++allocation) {
    Pair pair;
    auto callback = hp::app::MakeHttpFactory(service)();
    fail_after_registration = allocation;
    try {
      registry.AddConnection(Socket(pair.owned), std::move(callback));
      throw std::runtime_error("expected post-registration allocation failure");
    } catch (const std::bad_alloc&) {
      ++failures_seen;
    }
    fail_after_registration = -1;
    timer_allocation_failure = -1;
    Require(loop.timer_count() == 0 && Access::entries(registry).empty(),
            "adopt rollback");
    Require(::fcntl(pair.owned, F_GETFD) == -1 && errno == EBADF,
            "adopt socket closed");
  }
  Pair original;
  registry.AddConnection(Socket(original.owned),
                         hp::app::MakeHttpFactory(service)());
  const auto old_identity =
      Access::entries(registry).at(original.owned)->identity();
  const auto old_timer =
      Access::timer(*Access::entries(registry).at(original.owned));
  Access::entries(registry).at(original.owned)->RequestClose();
  registry.DrainClosedConnections();
  Pair replacement;
  Require(replacement.owned == original.owned, "real fd reused by socketpair");
  registry.AddConnection(Socket(replacement.owned),
                         hp::app::MakeHttpFactory(service)());
  auto& current = *Access::entries(registry).at(replacement.owned);
  Require(current.identity() != old_identity, "connection identity not reused");
  Require(!loop.CancelTimer(old_timer), "old timer cannot cancel new one");
  Access::Expire(registry, replacement.owned, old_identity);
  Require(current.state() == TcpConnection::State::kActive &&
              loop.timer_count() == 1,
          "old identity cannot close reused fd");
  bool callback_finished = false;

  using CloseExpiredConnectionTaskCurrentState = decltype((current));
  using CloseExpiredConnectionTaskRegistryState = decltype((registry));
  using CloseExpiredConnectionTaskCallbackFinishedState =
      decltype((callback_finished));
  struct CloseExpiredConnectionTask {
    CloseExpiredConnectionTaskCurrentState current;
    CloseExpiredConnectionTaskRegistryState registry;
    CloseExpiredConnectionTaskCallbackFinishedState callback_finished;
    decltype(auto) CloseExpiredConnection() const {
      current.RequestClose();
      Require(Access::entries(registry).size() == 1,
              "not destroyed inside callback");
      callback_finished = true;
    }
  };
  loop.AddTimer(
      Clock::now(),
      std::bind(
          &CloseExpiredConnectionTask::CloseExpiredConnection,
          CloseExpiredConnectionTask{current, registry, callback_finished}));
  loop.PollOnce(0);
  Require(callback_finished && Access::entries(registry).empty() &&
              loop.timer_count() == 0,
          "after_dispatch owns destruction");
  // A ready EOF and expired timer share one poll; IO closes first and cancels
  // its timer.
  Pair eof;
  registry.AddConnection(Socket(eof.owned),
                         hp::app::MakeHttpFactory(service)());
  loop.RescheduleTimer(Access::timer(*Access::entries(registry).at(eof.owned)),
                       Clock::now());
  ::shutdown(eof.peer.fd(), SHUT_WR);
  loop.PollOnce(0);
  Require(Access::entries(registry).empty() && loop.timer_count() == 0,
          "EOF and expiry once");
  std::cout << "rollback post_registration_failures=" << failures_seen
            << " reused_fd=" << replacement.owned << " old_id=" << old_identity
            << " callback_finished=" << callback_finished
            << " remaining=" << loop.timer_count() << '\n';
}

void FatalTimer(const hp::http::StaticFileService& service) {
  ServerHarness harness(service, 2, nullptr, {300ms, 200ms});
  Stream first(harness.port), second(harness.port);
  first.Send(Query("/note.txt"));
  second.Send(Query("/missing"));
  Response(first.ReadResponse(), 200, "hello from S3\n");
  Response(second.ReadResponse(), 404, "404 Not Found\n");
  std::promise<void> entered, release;
  auto gate = release.get_future().share();

  using WaitForTimerReleaseTaskEnteredState = decltype((entered));
  using WaitForTimerReleaseTaskGateState = decltype((gate));
  struct WaitForTimerReleaseTask {
    WaitForTimerReleaseTaskEnteredState entered;
    WaitForTimerReleaseTaskGateState gate;
    decltype(auto) WaitForTimerRelease(EventLoop&) const {
      entered.set_value();
      Require(gate.wait_for(3s) == std::future_status::ready,
              "fatal timer full worker gate");
    }
  };
  Require(TcpServerTestAccess::Post(
              *harness.server,
              1,
              std::bind(&WaitForTimerReleaseTask::WaitForTimerRelease,
                        WaitForTimerReleaseTask{entered, gate},
                        std::placeholders::_1)),
          "block other worker");
  Require(entered.get_future().wait_for(3s) == std::future_status::ready,
          "other worker entered");
  for (int i = 1; i < 1024; ++i)
    Require(TcpServerTestAccess::Post(*harness.server,
                                      1,
                                      &ConnectionTimeoutCompleteLoopProbe),
            "fill other worker");
  Require(!TcpServerTestAccess::Post(*harness.server,
                                     1,
                                     &ConnectionTimeoutCompleteLoopProbe),
          "capacity bounded");
  std::atomic<int> callbacks{0}, forbidden{0}, captures{0};

  using ScheduleFailingTimersTaskCallbacksState = decltype((callbacks));
  using ScheduleFailingTimersTaskCapturesState = decltype((captures));
  using ScheduleFailingTimersTaskForbiddenState = decltype((forbidden));
  struct ScheduleFailingTimersTask {
    ScheduleFailingTimersTaskCallbacksState callbacks;
    ScheduleFailingTimersTaskCapturesState captures;
    ScheduleFailingTimersTaskForbiddenState forbidden;
    decltype(auto) ScheduleFailingTimers(EventLoop& loop) const {
      struct Capture {
        EventLoop& loop;
        std::atomic<int>& released;

        ~Capture() {
          ++released;
          try {
            loop.AddTimer(Clock::now(), &ConnectionTimeoutCompleteQueuedProbe);
            std::terminate();
          } catch (const std::logic_error&) {
          }
        }
      };

      using ThrowTimerFailureTaskCallbacksState = decltype((callbacks));
      struct ThrowTimerFailureTask {
        ThrowTimerFailureTaskCallbacksState callbacks;
        decltype(auto) ThrowTimerFailure() const {
          ++callbacks;
          throw std::runtime_error("fatal timer original");
        }
      };
      loop.AddTimer(Clock::now(),
                    std::bind(&ThrowTimerFailureTask::ThrowTimerFailure,
                              ThrowTimerFailureTask{callbacks}));
      auto capture = std::shared_ptr<Capture>(new Capture{loop, captures});

      using RecordForbiddenExpiryTaskCaptureState =
          std::remove_cvref_t<decltype(capture)>;
      using RecordForbiddenExpiryTaskForbiddenState = decltype((forbidden));
      struct RecordForbiddenExpiryTask {
        RecordForbiddenExpiryTaskCaptureState capture;
        RecordForbiddenExpiryTaskForbiddenState forbidden;
        decltype(auto) RecordForbiddenExpiry() const { ++forbidden; }
      };
      loop.AddTimer(Clock::now() + 1h,
                    std::bind(&RecordForbiddenExpiryTask::RecordForbiddenExpiry,
                              RecordForbiddenExpiryTask{capture, forbidden}));
    }
  };
  Require(
      TcpServerTestAccess::Post(
          *harness.server,
          0,
          std::bind(&ScheduleFailingTimersTask::ScheduleFailingTimers,
                    ScheduleFailingTimersTask{callbacks, captures, forbidden},
                    std::placeholders::_1)),
      "install real worker timers");
  WaitUntil([&] { return Access::stopping(*harness.server); });
  release.set_value();
  harness.ExpectFailure("fatal timer original");
  Require(callbacks == 1 && forbidden == 0 && captures == 1,
          "fatal timer terminal cleanup");
  first.ExpectEof();
  second.ExpectEof();
  std::cout << "fatal_timer full_other_worker=1024 callbacks=" << callbacks
            << " forbidden=" << forbidden << " capture_release=" << captures
            << '\n';
}

void ActiveLifecycle(const hp::http::StaticFileService& service) {
  const auto fd = Resources("/proc/self/fd");
  const auto threads = Resources("/proc/self/task");
  std::size_t timers = 0;
  for (int cycle = 0; cycle < 100; ++cycle) {
    ServerHarness harness(service, 2, nullptr, {300ms, 200ms});
    Stream first(harness.port), second(harness.port);
    first.Send(Query("/note.txt"));
    second.Send(Query("/missing"));
    Response(first.ReadResponse(), 200, "hello from S3\n");
    Response(second.ReadResponse(), 404, "404 Not Found\n");
    for (std::size_t index = 0; index < 2; ++index) {
      const auto state = CollectTimeoutSnapshot(harness, 2, index);
      Require(state.connections == 1 && state.timers == 1,
              "active timer per worker");
      timers += state.timers;
    }
    harness.Stop();
    first.ExpectEof();
    second.ExpectEof();
  }
  WaitUntil([&] { return Resources("/proc/self/task") == threads; });
  Require(Resources("/proc/self/fd") == fd, "timer cycles fd baseline");
  std::cout << "active_lifecycle cycles=100 observed_timers=" << timers
            << " fd=" << fd << '/' << Resources("/proc/self/fd")
            << " threads=" << threads << '/' << Resources("/proc/self/task")
            << '\n';
}

void OptionsBoundaries() {
  struct ParseOptionValuesTask {
    decltype(auto) ParseOptionValues(std::vector<std::string> values) const {
      std::vector<char*> argv;
      for (auto& value : values) argv.push_back(value.data());
      return ParseServerOptions(static_cast<int>(argv.size()), argv.data());
    }
  };
  auto parse = ParseOptionValuesTask{};
  const auto defaults =
      parse.ParseOptionValues({"server", "--port", "0", "--root", "."});
  Require(defaults.shutdown_timeout == 5000ms,
          "actual production shutdown default");
  Require(defaults.timeouts.idle == 30000ms &&
              defaults.timeouts.keep_alive == 15000ms,
          "actual production parser defaults");
  TcpServer component(0);
  Require(Access::config(component).idle == 0ms &&
              Access::config(component).keep_alive == 0ms,
          "C++ compatibility defaults");
  int rejected = 0, accepted = 0;
  for (const auto option : {"--idle-timeout-ms", "--keep-alive-timeout-ms"}) {
    for (const auto value : {"0", "1", "86400000"}) {
      auto config = parse.ParseOptionValues(
          {"server", "--port", "0", "--root", ".", option, value});
      Require((std::string_view(option) == "--idle-timeout-ms"
                   ? config.timeouts.idle
                   : config.timeouts.keep_alive)
                      .count() == std::stoll(value),
              "valid timeout");
      ++accepted;
    }
    for (const auto value :
         {"86400001", "-1", "+1", "", "1x", "184467440737095516160"}) {
      try {
        (void)parse.ParseOptionValues(
            {"server", "--port", "0", "--root", ".", option, value});
        throw std::runtime_error("invalid timeout accepted");
      } catch (const std::invalid_argument&) {
        ++rejected;
      }
    }
    for (const auto& values :
         {std::vector<std::string>{"server", option},
          std::vector<std::string>{"server", option, "0", option, "0"}}) {
      try {
        (void)parse.ParseOptionValues(values);
        throw std::runtime_error("missing/repeated accepted");
      } catch (const std::invalid_argument&) {
        ++rejected;
      }
    }
  }
  std::cout << "options defaults_ms=" << defaults.timeouts.idle.count() << '/'
            << defaults.timeouts.keep_alive.count()
            << " cpp_ms=0/0 accepted=" << accepted << " rejected=" << rejected
            << '\n';
}
}  // namespace

int main() {
  try {
    Fixture fixture;
    hp::http::StaticFileService service(fixture.root_.string());
    watched_root = hp::http::StaticFileServiceTestAccess::root_fd(service);
    hp::app::set_RecordSessionEvent_callback(RecordSessionEvent);
    OptionsBoundaries();
    IdleModes(service);
    WaitingStates(service);
    Protocols(service, {300ms, 150ms});
    OwnerProgressAndFailures(service);
    WriteProgress(service);
    PacedInput(service);
    HealthyDuringTimeout(service);
    RollbackAndReuse(service);
    FatalTimer(service);
    ActiveLifecycle(service);
    hp::app::set_RecordSessionEvent_callback(nullptr);
    Require(root_errors == 0 && live_sessions == 0,
            "service outlives timer callbacks");
    Require(accepted_sockets == closed_sockets && invalid_closes == 0,
            "exact socket closes");
    std::cout << "close accepted=" << accepted_sockets
              << " closed=" << closed_sockets << " invalid=" << invalid_closes
              << " session_events=" << root_checks << '\n';
    std::cout << "connection_timeout_tests: PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
