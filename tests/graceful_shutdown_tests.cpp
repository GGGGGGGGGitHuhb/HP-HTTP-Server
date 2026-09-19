#include <functional>
#include <type_traits>
#define HP_MULTI_REACTOR_ENTRY legacy_graceful_multi_entry
#include "multi_reactor_tests.cpp"
#undef HP_MULTI_REACTOR_ENTRY
#include <sys/signalfd.h>
#include <sys/syscall.h>

#include "signal_watcher.h"

namespace hp::net {
struct GracefulShutdownTestAccess {
  static std::size_t finished(TcpServer& server) {
    return server.workers_finished_.load();
  }

  static auto& entries(ConnectionRegistry& registry) {
    return registry.connections_;
  }

  static EventLoop& main_loop(TcpServer& server) { return server.loop_; }

  static bool draining(TcpServer& server, std::size_t index) {
    auto& registry = server.worker_count_ ? *server.registries_[index]
                                          : *server.main_registry_;
    return registry.draining_;
  }

  struct ConnectionSnapshot {
    int fd, peer_error;
    TcpConnection::Identity identity;
    sockaddr_in peer;
    TcpConnection::State state;
    std::size_t pending, input;
    std::thread::id owner;
  };

  static std::vector<ConnectionSnapshot> Snapshot(TcpServer& server,
                                                  std::size_t index) {
    auto& registry = server.worker_count_ ? *server.registries_[index]
                                          : *server.main_registry_;
    std::vector<ConnectionSnapshot> result;
    for (auto& [fd, connection] : registry.connections_) {
      sockaddr_in peer{};
      socklen_t size = sizeof(peer);
      const int error =
          ::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &size) == 0
              ? 0
              : errno;
      result.push_back({fd,
                        error,
                        connection->identity(),
                        peer,
                        connection->state(),
                        connection->pending_bytes(),
                        connection->input_view().size(),
                        std::this_thread::get_id()});
    }
    return result;
  }

  static std::size_t pending(TcpServer& server, std::size_t index) {
    auto& registry = server.worker_count_ ? *server.registries_[index]
                                          : *server.main_registry_;
    std::size_t result = 0;
    for (auto& [fd, connection] : registry.connections_) {
      (void)fd;
      result += connection->pending_bytes();
    }
    return result;
  }
};
}  // namespace hp::net

namespace {
void GracefulShutdownCompleteQueuedProbe() {}
void GracefulShutdownCompleteLoopProbe(EventLoop&) {}

using Clock = hp::timer::TimerQueue::Clock;
using ShutdownAccess = GracefulShutdownTestAccess;

std::size_t Pending(ServerHarness& harness,
                    std::size_t workers,
                    std::size_t index = 0) {
  std::promise<std::size_t> done;
  auto result = done.get_future();

  using CollectPendingBytesTaskDoneState = decltype((done));
  using CollectPendingBytesTaskHarnessState = decltype((harness));
  using CollectPendingBytesTaskIndexState = decltype((index));
  struct CollectPendingBytesTask {
    CollectPendingBytesTaskDoneState done;
    CollectPendingBytesTaskHarnessState harness;
    CollectPendingBytesTaskIndexState index;
    decltype(auto) CollectPendingBytes() const {
      done.set_value(ShutdownAccess::pending(*harness.server, index));
    }
  };
  auto inspect = std::bind(&CollectPendingBytesTask::CollectPendingBytes,
                           CollectPendingBytesTask{done, harness, index});
  if (workers)
    Require(TcpServerTestAccess::Post(*harness.server, index, inspect),
            "pending observer");
  else
    Require(TcpServerTestAccess::MainPost(*harness.server, inspect),
            "main pending observer");
  Require(result.wait_for(3s) == std::future_status::ready,
          "pending observer deadline");
  return result.get();
}

bool CollectDrainObservation(ServerHarness& harness, std::size_t workers) {
  std::promise<bool> done;
  auto result = done.get_future();

  using CollectDrainingStateTaskDoneState = decltype((done));
  using CollectDrainingStateTaskHarnessState = decltype((harness));
  struct CollectDrainingStateTask {
    CollectDrainingStateTaskDoneState done;
    CollectDrainingStateTaskHarnessState harness;
    decltype(auto) CollectDrainingState() const {
      done.set_value(ShutdownAccess::draining(*harness.server, 0));
    }
  };
  auto observe = std::bind(&CollectDrainingStateTask::CollectDrainingState,
                           CollectDrainingStateTask{done, harness});
  if (workers)
    Require(TcpServerTestAccess::Post(*harness.server, 0, observe),
            "drain observer accepted");
  else
    Require(TcpServerTestAccess::MainPost(*harness.server, observe),
            "single loop drain observer");
  Require(result.wait_for(3s) == std::future_status::ready,
          "owner drain observer deadline");
  return result.get();
}

void ObserveDrain(ServerHarness& harness, std::size_t workers) {
  WaitUntil(
      std::bind_front(&CollectDrainObservation, std::ref(harness), workers));
}

void JoinGraceful(ServerHarness& harness) {
  harness.Join();
  if (harness.error) std::rethrow_exception(harness.error);
}

enum class DrainProbe { kNormal, kReordered, kLegacy, kNoInput, kWrongPeer };

sockaddr_in ClientEndpoint(int fd) {
  sockaddr_in address{};
  socklen_t size = sizeof(address);
  Require(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0,
          "partial client endpoint");
  return address;
}

std::vector<ShutdownAccess::ConnectionSnapshot> OwnerConnections(
    ServerHarness& harness,
    std::size_t workers) {
  std::vector<ShutdownAccess::ConnectionSnapshot> all;
  for (std::size_t index = 0; index < std::max(workers, std::size_t{1});
       ++index) {
    auto done = std::make_shared<
        std::promise<std::vector<ShutdownAccess::ConnectionSnapshot>>>();
    auto result = done->get_future();

    using CollectConnectionSnapshotTaskDoneState =
        std::remove_cvref_t<decltype(done)>;
    using CollectConnectionSnapshotTaskIndexState =
        std::remove_cvref_t<decltype(index)>;
    using CollectConnectionSnapshotTaskServerState = decltype(harness.server);
    struct CollectConnectionSnapshotTask {
      CollectConnectionSnapshotTaskDoneState done;
      CollectConnectionSnapshotTaskIndexState index;
      CollectConnectionSnapshotTaskServerState server;
      decltype(auto) CollectConnectionSnapshot() const {
        done->set_value(ShutdownAccess::Snapshot(*server, index));
      }
    };
    auto inspect =
        std::bind(&CollectConnectionSnapshotTask::CollectConnectionSnapshot,
                  CollectConnectionSnapshotTask{done, index, harness.server});
    const bool accepted =
        workers ? TcpServerTestAccess::Post(*harness.server, index, inspect)
                : TcpServerTestAccess::MainPost(*harness.server, inspect);
    Require(accepted, "connection snapshot accepted");
    Require(result.wait_for(3s) == std::future_status::ready,
            "connection snapshot deadline");
    auto rows = result.get();
    all.insert(all.end(), rows.begin(), rows.end());
  }
  return all;
}

std::size_t WritingPending(ServerHarness& harness,
                           std::size_t workers,
                           const Stream& writing) {
  const auto expected = ClientEndpoint(writing.fd);
  const auto connections = OwnerConnections(harness, workers);
  std::size_t matches = 0, pending_bytes = 0;
  for (const auto& connection : connections) {
    if (connection.peer_error == 0 &&
        connection.peer.sin_port == expected.sin_port &&
        connection.peer.sin_addr.s_addr == expected.sin_addr.s_addr) {
      ++matches;
      pending_bytes = connection.pending;
    }
  }
  Require(matches <= 1, "writing endpoint unique");
  return pending_bytes;
}

void PartialHandshake(ServerHarness& harness,
                      Probe& probe,
                      std::size_t workers,
                      Stream& partial,
                      const sockaddr_in& expected,
                      DrainProbe mode) {
  const std::string marker = "GET /note.txt HTTP/1.1\r\nHost:";

  using InspectPartialRequestTaskProbeState = decltype((probe));
  using InspectPartialRequestTaskModeState = decltype((mode));
  using InspectPartialRequestTaskExpectedState = decltype((expected));
  using InspectPartialRequestTaskMarkerState = decltype((marker));
  using InspectPartialRequestTaskHarnessState = decltype((harness));
  struct InspectPartialRequestTask {
    InspectPartialRequestTaskProbeState probe;
    InspectPartialRequestTaskModeState mode;
    InspectPartialRequestTaskExpectedState expected;
    InspectPartialRequestTaskMarkerState marker;
    InspectPartialRequestTaskHarnessState harness;
    decltype(auto) InspectPartialRequest() const {
      std::lock_guard lock(probe.mutex);
      if (mode == DrainProbe::kLegacy)
        return probe.owners.size() == 3 && probe.owners[2] != std::thread::id{};
      std::size_t matches = 0;
      for (const auto& message : probe.messages) {
        if (message.peer_error == 0 &&
            message.peer.sin_addr.s_addr == expected.sin_addr.s_addr &&
            message.peer.sin_port == expected.sin_port &&
            message.owner != std::thread::id{} && message.completed &&
            message.state == TcpConnection::State::kActive && !message.eof &&
            message.input == marker)
          ++matches;
      }
      return probe.owners.size() == 3 && matches == 1 &&
             !harness.run_ended.load();
    }
  };
  auto inspect = std::bind(
      &InspectPartialRequestTask::InspectPartialRequest,
      InspectPartialRequestTask{probe, mode, expected, marker, harness});
  try {
    WaitUntil(inspect);
  } catch (...) {
    const auto failure = std::current_exception();
    int error = 0;
    socklen_t size = sizeof(error);
    const int error_result =
        ::getsockopt(partial.fd, SOL_SOCKET, SO_ERROR, &error, &size);
    char byte;
    const auto received = ::recv(partial.fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    const int receive_error = received < 0 ? errno : 0;
    std::cerr << "partial_failure workers=" << workers
              << " client_fd=" << partial.fd
              << " expected_peer=" << ntohl(expected.sin_addr.s_addr) << ':'
              << ntohs(expected.sin_port)
              << " run_ended=" << harness.run_ended.load()
              << " run_failed=" << harness.run_failed.load()
              << " socket_error_result=" << error_result
              << " socket_error=" << error << " peek=" << received
              << " peek_error=" << receive_error << '\n';
    {
      std::lock_guard lock(probe.mutex);
      std::cerr << "partial_slots count=" << probe.owners.size()
                << " owner_errors=" << probe.owner_errors << '\n';
      for (std::size_t i = 0; i < probe.messages.size(); ++i) {
        const auto& message = probe.messages[i];
        std::cerr << "partial_slot index=" << i << " fd=" << message.fd
                  << " identity=" << message.identity
                  << " peer=" << ntohl(message.peer.sin_addr.s_addr) << ':'
                  << ntohs(message.peer.sin_port)
                  << " peer_error=" << message.peer_error
                  << " owner=" << message.owner
                  << " state=" << static_cast<int>(message.state)
                  << " eof=" << message.eof
                  << " completed=" << message.completed
                  << " calls=" << message.calls
                  << " bytes=" << message.input.size()
                  << " marker=" << (message.input == marker) << '\n';
      }
    }
    try {
      for (const auto& connection : OwnerConnections(harness, workers)) {
        std::cerr << "partial_registry fd=" << connection.fd
                  << " identity=" << connection.identity
                  << " peer=" << ntohl(connection.peer.sin_addr.s_addr) << ':'
                  << ntohs(connection.peer.sin_port)
                  << " owner=" << connection.owner
                  << " state=" << static_cast<int>(connection.state)
                  << " pending=" << connection.pending
                  << " input=" << connection.input
                  << " peer_error=" << connection.peer_error << '\n';
      }
    } catch (const std::exception& diagnostic) {
      std::cerr << "partial_registry unavailable=" << diagnostic.what() << '\n';
    }
    harness.server->RequestStop();
    harness.Join();
    std::cerr << "partial_terminal joined=1 error="
              << (harness.error != nullptr) << '\n';
    if (harness.error) {
      try {
        std::rethrow_exception(harness.error);
      } catch (const std::exception& error) {
        std::cerr << "partial_terminal reason=" << error.what() << '\n';
      }
    }
    std::rethrow_exception(failure);
  }
  std::lock_guard lock(probe.mutex);
  for (std::size_t i = 0; i < probe.messages.size(); ++i) {
    const auto& message = probe.messages[i];
    if (message.peer.sin_port == expected.sin_port)
      std::cout << "partial_ready workers=" << workers << " slot=" << i
                << " fd=" << message.fd << " identity=" << message.identity
                << " peer_port=" << ntohs(expected.sin_port)
                << " owner=" << message.owner
                << " bytes=" << message.input.size()
                << " completed=" << message.completed << '\n';
  }
}

void LibraryDrain(const hp::http::StaticFileService& service,
                  const Fixture& fixture,
                  DrainProbe mode = DrainProbe::kNormal) {
  for (std::size_t workers : {0U, 1U, 2U}) {
    Probe probe;
    // Preparation has no idle deadline; deadline_drain separately verifies idle
    // cancellation.
    ServerHarness harness(service, workers, &probe);
    std::optional<Stream> first;
    if (mode != DrainProbe::kNormal) first.emplace(harness.port);
    Stream writing(harness.port), idle(harness.port);
    std::optional<Stream> last;
    if (!first) last.emplace(harness.port);
    Stream& partial = first ? *first : *last;
    if (mode == DrainProbe::kReordered) {
      const auto endpoint = ClientEndpoint(partial.fd);
      partial.Send("GET /note");
      WaitUntil([&] {
        std::lock_guard lock(probe.mutex);
        return std::any_of(probe.messages.begin(),
                           probe.messages.end(),
                           [&](const auto& row) {
                             return row.peer.sin_port == endpoint.sin_port &&
                                    row.completed && row.input == "GET /note";
                           });
      });
      partial.Send(".txt HTTP/1.1\r\nHost:");
    } else if (mode != DrainProbe::kNoInput) {
      partial.Send("GET /note.txt HTTP/1.1\r\nHost:");
    }
    const auto endpoint =
        ClientEndpoint(mode == DrainProbe::kWrongPeer ? idle.fd : partial.fd);
    PartialHandshake(harness, probe, workers, partial, endpoint, mode);
    writing.Send(Query("/large.bin") + Query("/note.txt"));
    std::size_t bytes = 0;
    WaitUntil([&] {
      bytes = WritingPending(harness, workers, writing);
      return bytes > 0;
    });
    harness.server->RequestGracefulShutdown(Clock::now() + 2s);
    ObserveDrain(harness, workers);
    auto response = writing.ReadResponse();
    Require(
        response.status == 200 && response.body.size() == fixture.large_.size(),
        "drained size");
    Require(std::memcmp(response.body.data(),
                        fixture.large_.data(),
                        fixture.large_.size()) == 0,
            "8MiB response byte for byte");
    writing.ExpectEof();
    idle.ExpectEof();
    partial.ExpectEof();
    JoinGraceful(harness);
    std::cout << "library_drain workers=" << workers
              << " pending_before=" << bytes
              << " response_bytes=" << response.body.size()
              << " suffix_responses=0\n";
  }
}

void DeadlineDrain(const hp::http::StaticFileService& service,
                   const Fixture& fixture) {
  for (const auto timeout : {0ms, 120ms}) {
    ServerHarness harness(service, 2, nullptr, {30ms, 30ms});
    Stream writing(harness.port);
    writing.Send(Query("/large.bin") + Query("/note.txt"));
    WaitUntil([&] { return Pending(harness, 2) > 0; });
    const auto began = Clock::now();
    const auto deadline = began + timeout;
    harness.server->RequestGracefulShutdown(deadline);
    JoinGraceful(harness);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - began);
    Require(elapsed >= timeout && elapsed < 2s,
            "shared deadline instead of idle");
    std::string prefix;
    char bytes[65536];
    for (;;) {
      auto n = ::recv(writing.fd, bytes, sizeof(bytes), 0);
      if (n <= 0) break;
      prefix.append(bytes, static_cast<std::size_t>(n));
    }
    Require(prefix.starts_with("HTTP/1.1 200"), "original response prefix");
    Require(prefix.find("HTTP/1.1", 1) == std::string::npos,
            "no appended timeout/error response");
    Require(prefix.size() < hp::http::kMaxFileBytes,
            "deadline truncates stalled response");
    const auto body = prefix.find("\r\n\r\n") + 4;
    Require(body >= 4 && body <= prefix.size(),
            "complete response prefix header");
    Require(std::memcmp(prefix.data() + body,
                        fixture.large_.data(),
                        prefix.size() - body) == 0,
            "truncated body is byte-exact original prefix");
    std::cout << "deadline timeout_ms=" << timeout.count()
              << " elapsed_ms=" << elapsed.count()
              << " prefix_bytes=" << prefix.size() << '\n';
  }
}

void ProviderDrain(const hp::http::StaticFileService& service,
                   const Fixture& fixture) {
  EventLoop loop;
  ConnectionRegistry registry(loop, hp::http::kMaxRequestBytes, {30ms, 30ms});
  int sockets[2];
  Require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sockets) == 0,
          "provider pair");
  Socket peer(sockets[1]);
  int small = 4096, providers = 0;
  ::setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
  using ResponseScenario1State0 = decltype((providers));
  using ResponseScenario1State1 = decltype((service));
  struct ResponseScenario1 {
    ResponseScenario1State0 providers;
    ResponseScenario1State1 service;
    hp::http::ResponseResult PrepareResponse(
        const hp::http::HttpRequest& request,
        hp::http::ConnectionPolicy policy) {
      ++providers;
      return service.HandleResponse(request, policy);
    }
  };
  registry.AddConnection(Socket(sockets[0]),
                         hp::app::MakeHttpCallback(std::bind_front(
                             &ResponseScenario1::PrepareResponse,
                             ResponseScenario1{providers, service})));
  const auto request = Query("/large.bin") + Query("/note.txt");
  Require(::send(peer.fd(), request.data(), request.size(), MSG_NOSIGNAL) ==
              static_cast<ssize_t>(request.size()),
          "provider pipeline");
  loop.PollOnce(0);
  Require(providers == 1 && loop.timer_count() == 1, "one Writing provider");
  registry.BeginDrain();
  Require(loop.timer_count() == 0, "drain cancels idle and keep timers");
  std::string wire;
  char bytes[65536];
  const auto deadline = Clock::now() + 3s;
  for (;;) {
    auto n = ::recv(peer.fd(), bytes, sizeof(bytes), 0);
    if (n > 0)
      wire.append(bytes, static_cast<std::size_t>(n));
    else if (n == 0)
      break;
    loop.PollOnce(0);
    Require(Clock::now() < deadline, "provider drain deadline");
  }
  const auto body = wire.find("\r\n\r\n") + 4;
  Require(wire.size() - body == fixture.large_.size() &&
              std::memcmp(wire.data() + body,
                          fixture.large_.data(),
                          fixture.large_.size()) == 0,
          "provider drain full bytes");
  Require(providers == 1 && ShutdownAccess::entries(registry).empty(),
          "no suffix provider or retained connection");
  std::cout << "provider_drain calls=" << providers
            << " suffix_calls=0 body_bytes=" << wire.size() - body
            << " timer_count=" << loop.timer_count() << '\n';
}

void FullControl(const hp::http::StaticFileService& service) {
  ServerHarness harness(service, 2);
  std::promise<void> entered, release;
  auto gate = release.get_future().share();

  using WaitForMainReleaseTaskEnteredState = decltype((entered));
  using WaitForMainReleaseTaskGateState = decltype((gate));
  struct WaitForMainReleaseTask {
    WaitForMainReleaseTaskEnteredState entered;
    WaitForMainReleaseTaskGateState gate;
    decltype(auto) WaitForMainRelease() const {
      entered.set_value();
      Require(gate.wait_for(3s) == std::future_status::ready, "main gate");
    }
  };
  Require(TcpServerTestAccess::MainPost(
              *harness.server,
              std::bind(&WaitForMainReleaseTask::WaitForMainRelease,
                        WaitForMainReleaseTask{entered, gate})),
          "main blocker");
  Require(entered.get_future().wait_for(3s) == std::future_status::ready,
          "main entered");
  std::atomic<int> executed{0};

  using DrainAcceptedTaskIState = std::remove_cvref_t<int>;
  using DrainAcceptedTaskHarnessState = decltype((harness));
  using DrainAcceptedTaskExecutedState = decltype((executed));
  struct DrainAcceptedTaskTarget {
    DrainAcceptedTaskIState i;
    DrainAcceptedTaskHarnessState harness;
    DrainAcceptedTaskExecutedState executed;
    decltype(auto) DrainAcceptedTask() const {
      if (i == 1)
        WaitUntil(
            [&] { return ShutdownAccess::finished(*harness.server) == 2; });
      ++executed;
    }
  };
  for (int i = 1; i < 1024; ++i)
    Require(TcpServerTestAccess::MainPost(
                *harness.server,
                std::bind(&DrainAcceptedTaskTarget::DrainAcceptedTask,
                          DrainAcceptedTaskTarget{i, harness, executed})),
            "fill main");
  Require(!TcpServerTestAccess::MainPost(*harness.server,
                                         &GracefulShutdownCompleteQueuedProbe),
          "main saturated");
  harness.server->RequestGracefulShutdown(Clock::now() + 1s);
  release.set_value();
  JoinGraceful(harness);
  Require(executed == 1023,
          "main accepted tasks drain while worker done control bypasses limit");
  std::cout << "full_control main_outstanding=1024 executed_after_gate="
            << executed << '\n';
  ServerHarness saturated(service, 2);
  std::promise<void> handoff_entered, handoff_release;
  auto handoff_gate = handoff_release.get_future().share();

  using WaitForHandoffReleaseTaskHandoffEnteredState =
      decltype((handoff_entered));
  using WaitForHandoffReleaseTaskHandoffGateState = decltype((handoff_gate));
  struct WaitForHandoffReleaseTask {
    WaitForHandoffReleaseTaskHandoffEnteredState handoff_entered;
    WaitForHandoffReleaseTaskHandoffGateState handoff_gate;
    decltype(auto) WaitForHandoffRelease(EventLoop&) const {
      handoff_entered.set_value();
      Require(handoff_gate.wait_for(3s) == std::future_status::ready,
              "handoff full gate");
    }
  };
  Require(
      TcpServerTestAccess::Post(
          *saturated.server,
          1,
          std::bind(&WaitForHandoffReleaseTask::WaitForHandoffRelease,
                    WaitForHandoffReleaseTask{handoff_entered, handoff_gate},
                    std::placeholders::_1)),
      "handoff blocker");
  Require(
      handoff_entered.get_future().wait_for(3s) == std::future_status::ready,
      "handoff entered");
  for (int i = 1; i < 1024; ++i)
    Require(TcpServerTestAccess::Post(*saturated.server,
                                      1,
                                      &GracefulShutdownCompleteLoopProbe),
            "fill handoff worker");
  const auto closed_before = closed_sockets.load();
  Stream normal(saturated.port), rejected(saturated.port);
  rejected.ExpectEof();
  saturated.server->RequestGracefulShutdown(Clock::now() + 1s);
  normal.ExpectEof();
  handoff_release.set_value();
  JoinGraceful(saturated);
  Require(closed_sockets - closed_before == 2,
          "rejected handoff and normal fd closed once");
  std::cout << "full_handoff rejected=1 closed="
            << closed_sockets - closed_before << '\n';
  // A fatal on one owner still stops a full peer while graceful shutdown is
  // pending.
  ServerHarness fatal(service, 2);
  std::promise<void> peer_entered, peer_release;
  auto peer_gate = peer_release.get_future().share();

  using WaitForPeerReleaseTaskPeerEnteredState = decltype((peer_entered));
  using WaitForPeerReleaseTaskPeerGateState = decltype((peer_gate));
  struct WaitForPeerReleaseTask {
    WaitForPeerReleaseTaskPeerEnteredState peer_entered;
    WaitForPeerReleaseTaskPeerGateState peer_gate;
    decltype(auto) WaitForPeerRelease(EventLoop&) const {
      peer_entered.set_value();
      Require(peer_gate.wait_for(3s) == std::future_status::ready,
              "full peer gate");
    }
  };
  Require(TcpServerTestAccess::Post(
              *fatal.server,
              1,
              std::bind(&WaitForPeerReleaseTask::WaitForPeerRelease,
                        WaitForPeerReleaseTask{peer_entered, peer_gate},
                        std::placeholders::_1)),
          "peer blocker");
  Require(peer_entered.get_future().wait_for(3s) == std::future_status::ready,
          "peer entered");
  for (int i = 1; i < 1024; ++i)
    Require(TcpServerTestAccess::Post(*fatal.server,
                                      1,
                                      &GracefulShutdownCompleteLoopProbe),
            "fill peer");
  Require(!TcpServerTestAccess::Post(*fatal.server,
                                     1,
                                     &GracefulShutdownCompleteLoopProbe),
          "peer saturated");

  struct ThrowWorkerFailureTask {
    decltype(auto) ThrowWorkerFailure(EventLoop&) const {
      throw std::runtime_error("graceful fatal original");
    }
  };
  Require(TcpServerTestAccess::Post(
              *fatal.server,
              0,
              std::bind(&ThrowWorkerFailureTask::ThrowWorkerFailure,
                        ThrowWorkerFailureTask{},
                        std::placeholders::_1)),
          "fatal accepted");
  fatal.server->RequestGracefulShutdown(Clock::now() + 1s);
  peer_release.set_value();
  fatal.ExpectFailure("graceful fatal original");
  std::cout
      << "full_control peer_outstanding=1024 fatal_original_after_join=1\n";
}

using ThreadIds = std::set<pid_t>;

ThreadIds TaskIds() {
  ThreadIds result;
  for (const auto& entry :
       std::filesystem::directory_iterator("/proc/self/task"))
    result.insert(
        static_cast<pid_t>(std::stol(entry.path().filename().string())));
  return result;
}

std::string DescribeThreads(const ThreadIds& ids) {
  std::string result = "[";
  for (auto id : ids) result += std::to_string(id) + ",";
  return result + "]";
}

ThreadIds SettleThreads(const ThreadIds& owned,
                        const ThreadIds& expected,
                        const char* phase,
                        int cycle) {
  const auto start = Clock::now();
  const auto first = TaskIds();
  auto actual = first;
#if defined(__SANITIZE_THREAD__)
  constexpr std::size_t kBaselineSize = 2;
#else
  constexpr std::size_t kBaselineSize = 1;
#endif
  const auto controller = static_cast<pid_t>(::syscall(SYS_gettid));
  for (;;) {
    const bool retired =
        std::none_of(owned.begin(), owned.end(), [&](pid_t tid) {
          return actual.contains(tid);
        });
    const bool baseline = expected.empty() ? actual.size() == kBaselineSize &&
                                                 actual.contains(controller)
                                           : actual == expected;
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - start);
    if ((retired && baseline) || elapsed >= 3s) {
      std::cout << "thread_state phase=" << phase << " cycle=" << cycle
                << " expected_count="
                << (expected.empty() ? kBaselineSize : expected.size())
                << " expected=" << DescribeThreads(expected)
                << " owned=" << DescribeThreads(owned)
                << " first=" << DescribeThreads(first)
                << " actual=" << DescribeThreads(actual)
                << " elapsed_us=" << elapsed.count() << " retired=" << retired
                << std::endl;
      Require(retired && baseline, "thread identity baseline deadline");
      return actual;
    }
    std::this_thread::yield();
    actual = TaskIds();
  }
}

ThreadIds ReadyThreads(ServerHarness& harness) {
  ThreadIds result;
  for (int index : {-1, 0, 1}) {
    std::promise<pid_t> ready;
    auto tid = ready.get_future();

    using RecordOwnerThreadTaskReadyState = decltype((ready));
    struct RecordOwnerThreadTask {
      RecordOwnerThreadTaskReadyState ready;
      decltype(auto) RecordOwnerThread() const {
        ready.set_value(static_cast<pid_t>(::syscall(SYS_gettid)));
      }
    };
    auto observe = std::bind(&RecordOwnerThreadTask::RecordOwnerThread,
                             RecordOwnerThreadTask{ready});
    const bool accepted =
        index < 0 ? TcpServerTestAccess::MainPost(*harness.server, observe)
                  : TcpServerTestAccess::Post(*harness.server, index, observe);
    Require(accepted, "thread identity observer accepted");
    Require(tid.wait_for(3s) == std::future_status::ready,
            "thread identity ready deadline");
    Require(result.insert(tid.get()).second,
            "three distinct server owner TIDs");
  }
  Require(!result.contains(static_cast<pid_t>(::syscall(SYS_gettid))),
          "server owners exclude caller");
  return result;
}

ThreadIds MaskLifetime(bool startup_fault = true,
                       bool registration_fault = true) {
  sigset_t before, after;
  ::pthread_sigmask(SIG_SETMASK, nullptr, &before);
  const auto fds = Resources("/proc/self/fd");
  ThreadIds baseline;
  for (int mode = 0; mode < 3; ++mode) {
    ThreadIds owned;
    bool startup_failed = false, registration_failed = false;
    const auto consumed_before = registration_injections;
    {
      hp::app::SignalWatcher watcher;
      ::pthread_sigmask(SIG_SETMASK, nullptr, &after);
      Require(::sigismember(&after, SIGINT) == 1 &&
                  ::sigismember(&after, SIGTERM) == 1,
              "signals blocked before workers");
      Require((::fcntl(watcher.fd(), F_GETFD) & FD_CLOEXEC) &&
                  (::fcntl(watcher.fd(), F_GETFL) & O_NONBLOCK),
              "signalfd flags");
      if (mode == 1) {
        try {
          TcpServer invalid(0, 0, startup_fault ? 65 : 0);
        } catch (const std::invalid_argument& error) {
          Require(std::string(error.what()) == "worker count exceeds 64",
                  "exact server startup validation error");
          startup_failed = true;
        }
      } else {
        TcpServer server(0, 0, 2);
        for (std::size_t index = 0; index < 2; ++index) {
          std::promise<std::pair<bool, pid_t>> observed_mask;
          auto result = observed_mask.get_future();

          using CollectSignalMaskTaskObservedMaskState =
              decltype((observed_mask));
          struct CollectSignalMaskTask {
            CollectSignalMaskTaskObservedMaskState observed_mask;
            decltype(auto) CollectSignalMask(EventLoop&) const {
              sigset_t current;
              ::pthread_sigmask(SIG_SETMASK, nullptr, &current);
              observed_mask.set_value(
                  {::sigismember(&current, SIGINT) == 1 &&
                       ::sigismember(&current, SIGTERM) == 1,
                   static_cast<pid_t>(::syscall(SYS_gettid))});
            }
          };
          Require(TcpServerTestAccess::Post(
                      server,
                      index,
                      std::bind(&CollectSignalMaskTask::CollectSignalMask,
                                CollectSignalMaskTask{observed_mask},
                                std::placeholders::_1)),
                  "observe inherited worker mask");
          Require(result.wait_for(3s) == std::future_status::ready,
                  "worker mask observer ready");
          const auto [blocked, tid] = result.get();
          Require(blocked, "worker inherits blocked shutdown signals");
          Require(owned.insert(tid).second, "distinct mask worker identities");
        }
        if (mode == 2) reject_any_registration = registration_fault;
        try {
          struct PassiveSignalObserver {
            static void HandleShutdownSignal(std::uint32_t) {}
          };
          auto& signal_channel = server.WatchControlFd(watcher.fd());
          signal_channel.set_HandleShutdownSignal_callback(
              &PassiveSignalObserver::HandleShutdownSignal);
          signal_channel.set_interest(EPOLLIN);
        } catch (const std::system_error& error) {
          Require(
              mode == 2 &&
                  error.code() == std::error_code(EIO, std::generic_category()),
              "exact signal registration EIO");
          registration_failed = true;
        }
      }
    }
    ::pthread_sigmask(SIG_SETMASK, nullptr, &after);
    for (int signal = 1; signal < NSIG; ++signal)
      Require(::sigismember(&before, signal) == ::sigismember(&after, signal),
              "original mask restored");
    Require(Resources("/proc/self/fd") == fds,
            "watcher/server failure fd rollback");
    baseline = SettleThreads(owned, baseline, "mask", mode);
    const auto consumed = registration_injections - consumed_before;
    std::cout << "mask_scope mode=" << mode
              << " startup_error=" << startup_failed
              << " registration_error=" << registration_failed
              << " injection_consumed=" << consumed
              << " mask_restored=1 fd=" << fds << '/'
              << Resources("/proc/self/fd") << std::endl;
    Require(startup_failed == (mode == 1), "startup failure not detected");
    Require(registration_failed == (mode == 2),
            "registration injection missed");
    Require(consumed == (mode == 2 ? 1U : 0U),
            "registration injection consumed exactly once");
  }
  std::cout << "mask_lifetime scopes=3 registration_failure=1 fd=" << fds << '/'
            << Resources("/proc/self/fd") << '\n';
  return baseline;
}

void RepeatedLifecycle(const hp::http::StaticFileService& service,
                       const ThreadIds& expected) {
  const auto baseline = SettleThreads({}, expected, "baseline", -1);
  const auto fds = Resources("/proc/self/fd"), threads = baseline.size();
  const auto before = closed_sockets.load();
  for (int cycle = 0; cycle < 100; ++cycle) {
    ServerHarness harness(service, 2, nullptr, {300ms, 200ms});
    const auto owned = ReadyThreads(harness);
    auto ready = baseline;
    ready.insert(owned.begin(), owned.end());
    Require(TaskIds() == ready,
            "ready thread identity set matches three owners");
    Stream first(harness.port), second(harness.port);
    first.Send(Query("/note.txt"));
    second.Send(Query("/missing"));
    Response(first.ReadResponse(), 200, "hello from S3\n");
    Response(second.ReadResponse(), 404, "404 Not Found\n");
    harness.server->RequestGracefulShutdown(Clock::now() + 200ms);
    first.ExpectEof();
    second.ExpectEof();
    JoinGraceful(harness);
    // join is the owner lifetime handshake; proc removal can finish just after
    // it.
    SettleThreads(owned, baseline, "joined", cycle);
  }
  Require(TaskIds() == baseline, "100 graceful exact thread identity baseline");
  Require(Resources("/proc/self/task") == threads,
          "100 graceful thread count baseline");
  Require(Resources("/proc/self/fd") == fds, "100 graceful fd baseline");
  std::cout << "graceful_lifecycle cycles=100 closed="
            << closed_sockets - before << " fd=" << fds << '/'
            << Resources("/proc/self/fd") << " threads=" << threads << '/'
            << Resources("/proc/self/task") << '\n';
}

struct Child {
  pid_t pid{-1};
  int output{-1};
  std::uint16_t port{};
  std::string text;

  Child(const char* executable,
        const Fixture& fixture,
        int workers,
        int timeout) {
    int pipes[2];
    Require(::pipe2(pipes, O_CLOEXEC) == 0, "child pipe");
    std::vector<std::string> args{executable,
                                  "--port",
                                  "0",
                                  "--root",
                                  fixture.root_.string(),
                                  "--threads",
                                  std::to_string(workers),
                                  "--shutdown-timeout-ms",
                                  std::to_string(timeout),
                                  "--idle-timeout-ms",
                                  "0"};
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid = ::fork();
    Require(pid >= 0, "child fork");
    if (!pid) {
      ::close(pipes[0]);
      ::dup2(pipes[1], STDOUT_FILENO);
      ::dup2(pipes[1], STDERR_FILENO);
      ::close(pipes[1]);
      ::execv(executable, argv.data());
      _exit(127);
    }
    ::close(pipes[1]);
    output = pipes[0];
    ::fcntl(output, F_SETFL, O_NONBLOCK);
    try {
      WaitUntil(std::bind_front(&Child::ReadStartupReady, this));
    } catch (...) {
      Reap();
      throw;
    }
  }

  bool ReadStartupReady() {
    ReadOutput();
    const std::string marker = "server listening on port ";
    const auto at = text.find(marker);
    if (at == std::string::npos) return false;
    port =
        static_cast<std::uint16_t>(std::stoul(text.substr(at + marker.size())));
    return port != 0;
  }

  ~Child() { Reap(); }

  void ReadOutput() {
    char bytes[4096];
    for (;;) {
      auto count = ::read(output, bytes, sizeof(bytes));
      if (count <= 0) break;
      text.append(bytes, static_cast<std::size_t>(count));
    }
  }

  void Reap() noexcept {
    if (pid > 0) {
      ::kill(pid, SIGKILL);
      while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
      }
      pid = -1;
    }
    if (output >= 0) {
      ::close(output);
      output = -1;
    }
  }

  void Signal(int value) {
    Require(pid > 0 && ::kill(pid, value) == 0, "signal own known child");
  }

  void Wait() {
    int status = 0;
    WaitUntil([&] {
      ReadOutput();
      return ::waitpid(pid, &status, WNOHANG) == pid;
    });
    pid = -1;
    ReadOutput();
    Require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "normal application signal exit0");
    Require(text.find("Shutdown signal observed:") != std::string::npos,
            "application consumed signal");
  }
};

void ProcessSignals(const char* executable, const Fixture& fixture) {
  for (int workers : {0, 1, 2}) {
    for (int signal : {SIGINT, SIGTERM}) {
      Child child(executable, fixture, workers, 5000);
      Stream peer(child.port);
      peer.Send(Query("/note.txt"));
      Response(peer.ReadResponse(), 200, "hello from S3\n");
      const auto began = Clock::now();
      child.Signal(signal);
      peer.ExpectEof();
      child.Wait();
      Require(Clock::now() - began < 2s, "empty drain exits early");
      std::cout << "signal workers=" << workers << " signo=" << signal
                << " exit=0\n";
    }
  }
  for (int timeout : {0, 120}) {
    Child timed(executable, fixture, 2, timeout);
    Stream writing(timed.port);
    writing.Send(Query("/large.bin"));
    WaitUntil([&] {
      timed.ReadOutput();
      return timed.text.find("write reached EAGAIN") != std::string::npos;
    });
    const auto began = Clock::now();
    timed.Signal(SIGTERM);
    timed.Wait();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - began);
    Require(elapsed >= std::chrono::milliseconds(timeout) && elapsed < 2s,
            "production shutdown argument reaches owner deadline");
    std::cout << "signal_timeout configured_ms=" << timeout
              << " elapsed_ms=" << elapsed.count() << " exit=0\n";
  }
  Child child(executable, fixture, 2, 5000);
  Stream stalled(child.port);
  stalled.Send(Query("/large.bin"));
  WaitUntil([&] {
    child.ReadOutput();
    return child.text.find("write reached EAGAIN") != std::string::npos;
  });
  child.Signal(SIGTERM);
  WaitUntil([&] {
    child.ReadOutput();
    return child.text.find("Shutdown signal observed:") != std::string::npos;
  });
  const auto began = Clock::now();
  child.Signal(SIGINT);
  child.Wait();
  Require(Clock::now() - began < 2s, "second observed signal forces close");
  std::cout << "second_signal exit=0 forced_before_5000ms=1\n";
}
}  // namespace

#ifndef HP_GRACEFUL_ENTRY
#define HP_GRACEFUL_ENTRY main
#endif
int HP_GRACEFUL_ENTRY(int argc, char** argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--mask-no-startup-fault") {
      MaskLifetime(false, true);
      return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--mask-no-registration-fault") {
      MaskLifetime(true, false);
      return 0;
    }
    Fixture fixture;
    hp::http::StaticFileService service(fixture.root_.string());
    watched_root = hp::http::StaticFileServiceTestAccess::root_fd(service);
    hp::app::set_RecordSessionEvent_callback(RecordSessionEvent);
    if (argc == 2 && std::string_view(argv[1]).starts_with("--drain-")) {
      const std::string_view option(argv[1]);
      const auto mode = option == "--drain-legacy"     ? DrainProbe::kLegacy
                        : option == "--drain-no-input" ? DrainProbe::kNoInput
                        : option == "--drain-wrong-peer"
                            ? DrainProbe::kWrongPeer
                            : DrainProbe::kReordered;
      LibraryDrain(service, fixture, mode);
      return 0;
    }
    LibraryDrain(service, fixture);
    DeadlineDrain(service, fixture);
    ProviderDrain(service, fixture);
    FullControl(service);
    const auto baseline = MaskLifetime();
    RepeatedLifecycle(service, baseline);
    if (argc == 2) ProcessSignals(argv[1], fixture);
    hp::app::set_RecordSessionEvent_callback(nullptr);
    Require(root_errors == 0 && live_sessions == 0, "service lifetime");
    Require(accepted_sockets == closed_sockets && invalid_closes == 0,
            "exact accepted closes");
    std::cout << "close accepted=" << accepted_sockets
              << " closed=" << closed_sockets << " invalid=" << invalid_closes
              << " session_events=" << root_checks << '\n';
    std::cout << "graceful_shutdown_tests: PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
