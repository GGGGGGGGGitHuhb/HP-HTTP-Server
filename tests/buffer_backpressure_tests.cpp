#include <functional>
#include <type_traits>
#define HP_SENDFILE_ENTRY legacy_buffer_sendfile_entry
#include "sendfile_tests.cpp"
#undef HP_SENDFILE_ENTRY

namespace {
struct ReceiveEvidence {
  std::uintptr_t address{};
  std::size_t bytes{}, calls{}, checked{};
};
std::mutex receive_mutex;
std::map<int, ReceiveEvidence> receive_evidence;
std::atomic<std::size_t> direct_hits{}, direct_errors{};

void DirectProbe(TcpConnection& connection,
                 std::span<const std::byte> bytes,
                 bool) {
  std::lock_guard lock(receive_mutex);
  auto& record = receive_evidence[connection.fd()];
  if (record.calls == record.checked) return;
  const auto begin = reinterpret_cast<std::uintptr_t>(bytes.data());
  if (record.address < begin || record.address - begin > bytes.size() ||
      record.bytes > bytes.size() - (record.address - begin))
    ++direct_errors;
  ++direct_hits;
  record.checked = record.calls;
}

struct BufferSnapshot {
  int fd{-1};
  TcpConnection::Identity identity{};
  std::thread::id owner;
  std::size_t pending{}, input{}, input_capacity{}, output_capacity{}, calls{};
  bool paused{};
};

BufferSnapshot Snapshot(ServerHarness& harness,
                        std::size_t workers,
                        const sockaddr_in& peer) {
  BufferSnapshot found;
  for (std::size_t index = 0; index < std::max(std::size_t{1}, workers);
       ++index) {
    std::promise<BufferSnapshot> promise;
    auto result = promise.get_future();

    using CollectBufferSnapshotTaskHarnessState = decltype((harness));
    using CollectBufferSnapshotTaskIndexState = decltype((index));
    using CollectBufferSnapshotTaskPeerState = decltype((peer));
    using CollectBufferSnapshotTaskPromiseState = decltype((promise));
    struct CollectBufferSnapshotTask {
      CollectBufferSnapshotTaskHarnessState harness;
      CollectBufferSnapshotTaskIndexState index;
      CollectBufferSnapshotTaskPeerState peer;
      CollectBufferSnapshotTaskPromiseState promise;
      decltype(auto) CollectBufferSnapshot() const {
        BufferSnapshot value;
        auto& entries = ShutdownAccess::entries(
            SendfileTestAccess::registry(*harness.server, index));
        for (auto& [fd, connection] : entries) {
          sockaddr_in actual{};
          socklen_t size = sizeof(actual);
          if (::getpeername(fd, reinterpret_cast<sockaddr*>(&actual), &size) !=
                  0 ||
              actual.sin_port != peer.sin_port ||
              actual.sin_addr.s_addr != peer.sin_addr.s_addr)
            continue;
          value.fd = fd;
          value.identity = connection->identity();
          value.owner = std::this_thread::get_id();
          value.pending = connection->pending_bytes();
          value.input = connection->input_view().size();
          value.input_capacity =
              SendfileTestAccess::input_capacity(*connection);
          value.output_capacity = SendfileTestAccess::capacity(*connection);
          value.paused = SendfileTestAccess::paused(*connection);
          std::lock_guard lock(receive_mutex);
          value.calls = receive_evidence[fd].calls;
        }
        promise.set_value(value);
      }
    };
    auto inspect =
        std::bind(&CollectBufferSnapshotTask::CollectBufferSnapshot,
                  CollectBufferSnapshotTask{harness, index, peer, promise});
    if (workers)
      Require(TcpServerTestAccess::Post(*harness.server, index, inspect),
              "owner snapshot queued");
    else
      Require(TcpServerTestAccess::MainPost(*harness.server, inspect),
              "main snapshot queued");
    Require(result.wait_for(3s) == std::future_status::ready,
            "owner snapshot completion handshake");
    const auto value = result.get();
    if (value.fd >= 0) {
      Require(found.fd < 0, "peer uniquely matches one live fd identity");
      found = value;
    }
  }
  Require(found.fd >= 0, "observed slow peer identity");
  return found;
}

void SlowPipeline(const Fixture& fixture,
                  const hp::http::StaticFileService& service,
                  std::size_t workers) {
  Probe probe;
  probe.before_message = DirectProbe;
  const auto hits = direct_hits.load();
  ServerHarness harness(service, workers, &probe);
  Stream slow(harness.port);
  sockaddr_in peer{};
  socklen_t size = sizeof(peer);
  Require(
      ::getsockname(slow.fd, reinterpret_cast<sockaddr*>(&peer), &size) == 0,
      "slow peer identity");
  const auto file_start = FileCount();
  slow.Send(Query("/large.bin") + Query("/note.txt"));
  WaitUntil([&] {
    return FileCount() > file_start && Evidence(file_start).eagain > 0;
  });
  const auto before = Snapshot(harness, workers, peer);
  Require(before.pending > 0 && before.paused &&
              before.input_capacity <= 16384 && before.output_capacity <= 65536,
          "Writing paused with bounded Buffer after real EAGAIN");
  for (int i = 0; i < 8; ++i) slow.Send(Query("/note.txt"));
  slow.Send(Query("/missing", true));
  // With two workers, two healthy connections include one on the slow owner.
  for (std::size_t i = 0; i < std::max(std::size_t{1}, workers); ++i) {
    Stream healthy(harness.port);
    healthy.Send(Query("/note.txt", true));
    Response(healthy.ReadResponse(), 200, "hello from S3\n");
    healthy.ExpectEof();
  }
  bool healthy_same_owner = false;
  {
    std::lock_guard lock(probe.mutex);
    for (const auto& message : probe.messages)
      if (message.completed && message.peer_error == 0 &&
          message.peer.sin_port != peer.sin_port &&
          message.owner == before.owner)
        healthy_same_owner = true;
  }
  Require(healthy_same_owner,
          "healthy connection on the actual slow owner completed");
  const auto held = Snapshot(harness, workers, peer);
  Require(held.fd == before.fd && held.identity == before.identity &&
              held.owner == before.owner,
          "stable fd identity owner during backpressure");
  Require(held.paused && held.pending > 0 && held.input == before.input &&
              held.calls == before.calls,
          "Writing prevents recv and pipeline advancement while healthy "
          "control completes");
  const auto first = slow.ReadResponse();
  Require(first.status == 200 && first.body.size() == fixture.large_.size() &&
              std::memcmp(first.body.data(),
                          fixture.large_.data(),
                          fixture.large_.size()) == 0,
          "slow first response exact bytes");
  for (int i = 0; i < 9; ++i)
    Response(slow.ReadResponse(), 200, "hello from S3\n");
  Response(slow.ReadResponse(), 404, "404 Not Found\n");
  slow.ExpectEof();
  harness.Stop();
  Require(direct_hits > hits && direct_errors == 0,
          "production recv address belongs to committed Buffer");
  std::cout
      << "Buffer production workers=" << workers
      << " peer=" << ntohs(peer.sin_port) << " fd=" << before.fd
      << " identity=" << before.identity << " owner=" << before.owner
      << " input_capacity=" << before.input_capacity
      << " recv_held=" << held.calls
      << " healthy/control_before_release=1 ordered_responses=11 direct_recv="
      << direct_hits - hits << " PASS\n";
}

void BufferLifecycle(const hp::http::StaticFileService& service) {
  ScopedAcceptedSendBuffer send_buffer_scope;
  const auto base_fds = Resources("/proc/self/fd");
  const auto base_threads = TaskIds();
  for (std::size_t workers : {0U, 1U, 2U}) {
    for (int cycle = 0; cycle < 100; ++cycle) {
      Probe probe;
      probe.before_message = DirectProbe;
      {
        ServerHarness harness(service, workers, &probe);
        Stream slow(harness.port);
        const auto opening = FileCount();
        slow.Send(Query("/large.bin") + Query("/note.txt"));
        WaitUntil([&] {
          return FileCount() > opening && Evidence(opening).eagain > 0;
        });
        linger reset{1, 0};
        ::setsockopt(slow.fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
        ::shutdown(slow.fd, SHUT_RDWR);
        harness.server->ForceShutdown();
        JoinGraceful(harness);
      }
      Require(Resources("/proc/self/fd") == base_fds,
              "Buffer lifecycle fd returns to baseline");
    }
    WaitUntil([&] { return TaskIds() == base_threads; });
    std::cout << "Buffer lifecycle workers=" << workers
              << " cycles=100 slow/error/shutdown fd_baseline=" << base_fds
              << " threads_restored=1 PASS\n";
  }
  send_buffer_scope.VerifyAndRestore();
}
}  // namespace

extern "C" ssize_t __real_recv(int, void*, std::size_t, int);

extern "C" ssize_t __wrap_recv(int fd,
                               void* data,
                               std::size_t size,
                               int flags) {
  const auto result = __real_recv(fd, data, size, flags);
  if (result > 0) {
    std::lock_guard lock(receive_mutex);
    auto& record = receive_evidence[fd];
    record.address = reinterpret_cast<std::uintptr_t>(data);
    record.bytes = result;
    ++record.calls;
  }
  return result;
}

int main(int argc, char** argv) {
  try {
    Fixture fixture;
    hp::http::StaticFileService service(fixture.root_.string());
    const auto mode =
        argc == 2 ? std::string_view(argv[1]) : std::string_view{};
    for (std::size_t workers : {0U, 1U, 2U}) {
      ScopedAcceptedSendBuffer send_buffer_scope;
      SlowPipeline(fixture, service, workers);
      send_buffer_scope.VerifyAndRestore();
    }
    if (mode != "--pipeline-only") BufferLifecycle(service);
    Require(direct_errors == 0 && active_files.empty(),
            "all observed files closed and direct recv valid");
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "buffer production assertion: " << e.what() << '\n';
    return 1;
  }
}
