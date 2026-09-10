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

void direct_probe(TcpConnection& connection, std::span<const std::byte> bytes,
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

BufferSnapshot snapshot(ServerHarness& harness, std::size_t workers,
                        const sockaddr_in& peer) {
  BufferSnapshot found;
  for (std::size_t index = 0; index < std::max(std::size_t{1}, workers);
       ++index) {
    std::promise<BufferSnapshot> promise;
    auto result = promise.get_future();
    auto inspect = [&] {
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
        value.input_capacity = SendfileTestAccess::input_capacity(*connection);
        value.output_capacity = SendfileTestAccess::capacity(*connection);
        value.paused = SendfileTestAccess::paused(*connection);
        std::lock_guard lock(receive_mutex);
        value.calls = receive_evidence[fd].calls;
      }
      promise.set_value(value);
    };
    if (workers)
      require(TcpServerTestAccess::post(*harness.server, index,
                                        [&](EventLoop&) { inspect(); }),
              "owner snapshot queued");
    else
      require(TcpServerTestAccess::main_post(*harness.server, inspect),
              "main snapshot queued");
    require(result.wait_for(3s) == std::future_status::ready,
            "owner snapshot completion handshake");
    const auto value = result.get();
    if (value.fd >= 0) {
      require(found.fd < 0, "peer uniquely matches one live fd identity");
      found = value;
    }
  }
  require(found.fd >= 0, "observed slow peer identity");
  return found;
}

void slow_pipeline(const Fixture& fixture,
                   const hp::http::StaticFileService& service,
                   std::size_t workers) {
  Probe probe;
  probe.before_message = direct_probe;
  const auto hits = direct_hits.load();
  ServerHarness harness(service, workers, &probe);
  Stream slow(harness.port);
  sockaddr_in peer{};
  socklen_t size = sizeof(peer);
  require(
      ::getsockname(slow.fd, reinterpret_cast<sockaddr*>(&peer), &size) == 0,
      "slow peer identity");
  const auto file_start = file_count();
  slow.send(query("/large.bin") + query("/note.txt"));
  await([&] {
    return file_count() > file_start && evidence(file_start).eagain > 0;
  });
  const auto before = snapshot(harness, workers, peer);
  require(before.pending > 0 && before.paused &&
              before.input_capacity <= 16384 && before.output_capacity <= 65536,
          "Writing paused with bounded Buffer after real EAGAIN");
  for (int i = 0; i < 8; ++i) slow.send(query("/note.txt"));
  slow.send(query("/missing", true));
  // With two workers, two healthy connections include one on the slow owner.
  for (std::size_t i = 0; i < std::max(std::size_t{1}, workers); ++i) {
    Stream healthy(harness.port);
    healthy.send(query("/note.txt", true));
    response(healthy.next(), 200, "hello from S3\n");
    healthy.eof();
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
  require(healthy_same_owner,
          "healthy connection on the actual slow owner completed");
  const auto held = snapshot(harness, workers, peer);
  require(held.fd == before.fd && held.identity == before.identity &&
              held.owner == before.owner,
          "stable fd identity owner during backpressure");
  require(held.paused && held.pending > 0 && held.input == before.input &&
              held.calls == before.calls,
          "Writing prevents recv and pipeline advancement while healthy "
          "control completes");
  const auto first = slow.next();
  require(first.status == 200 && first.body.size() == fixture.large.size() &&
              std::memcmp(first.body.data(), fixture.large.data(),
                          fixture.large.size()) == 0,
          "slow first response exact bytes");
  for (int i = 0; i < 9; ++i) response(slow.next(), 200, "hello from S3\n");
  response(slow.next(), 404, "404 Not Found\n");
  slow.eof();
  harness.stop();
  require(direct_hits > hits && direct_errors == 0,
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

void buffer_lifecycle(const hp::http::StaticFileService& service) {
  const auto base_fds = resources("/proc/self/fd");
  const auto base_threads = task_ids();
  for (std::size_t workers : {0U, 1U, 2U}) {
    for (int cycle = 0; cycle < 100; ++cycle) {
      Probe probe;
      probe.before_message = direct_probe;
      {
        ServerHarness harness(service, workers, &probe);
        Stream slow(harness.port);
        const auto opening = file_count();
        slow.send(query("/large.bin") + query("/note.txt"));
        await([&] {
          return file_count() > opening && evidence(opening).eagain > 0;
        });
        linger reset{1, 0};
        ::setsockopt(slow.fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
        ::shutdown(slow.fd, SHUT_RDWR);
        harness.server->force_shutdown();
        join_graceful(harness);
      }
      require(resources("/proc/self/fd") == base_fds,
              "Buffer lifecycle fd returns to baseline");
    }
    await([&] { return task_ids() == base_threads; });
    std::cout << "Buffer lifecycle workers=" << workers
              << " cycles=100 slow/error/shutdown fd_baseline=" << base_fds
              << " threads_restored=1 PASS\n";
  }
}
}  // namespace

extern "C" ssize_t __real_recv(int, void*, std::size_t, int);

extern "C" ssize_t __wrap_recv(int fd, void* data, std::size_t size,
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
    hp::http::StaticFileService service(fixture.root.string());
    const auto mode =
        argc == 2 ? std::string_view(argv[1]) : std::string_view{};
    for (std::size_t workers : {0U, 1U, 2U})
      slow_pipeline(fixture, service, workers);
    if (mode != "--pipeline-only") buffer_lifecycle(service);
    require(direct_errors == 0 && active_files.empty(),
            "all observed files closed and direct recv valid");
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "buffer production assertion: " << e.what() << '\n';
    return 1;
  }
}
