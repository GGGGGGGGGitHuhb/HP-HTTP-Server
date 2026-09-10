// Reuse the accepted owner/TID, process, HTTP and fault fixtures without
// copying transport logic.
#define HP_GRACEFUL_ENTRY legacy_sendfile_graceful_entry
#define __wrap_close __wrap_socket_close
#include "graceful_shutdown_tests.cpp"
#undef __wrap_close
#undef HP_GRACEFUL_ENTRY
#include <poll.h>
#include <sys/sendfile.h>
#include <sys/stat.h>

#include <cstdarg>

namespace {
struct FileEvidence {
  int fd;
  dev_t device;
  ino_t inode;
  std::size_t closes{}, reads{}, preads{}, stats{}, calls{}, positive{},
      short_writes{}, eagain{};
  std::size_t bytes{}, injected{};
  bool offset_error{};
};

std::mutex file_evidence_mutex;
std::vector<FileEvidence> file_evidence;
std::map<int, std::size_t> active_files;
thread_local int sendfile_injection = 0;
thread_local unsigned int sendfile_injection_repeats = 0;
thread_local int mask_injection = 0;
thread_local unsigned int mask_injection_consumed = 0;
thread_local unsigned int sendfile_injection_consumed = 0;

FileEvidence evidence(std::size_t index) {
  std::lock_guard lock(file_evidence_mutex);
  return file_evidence.at(index);
}

std::size_t latest_file() {
  std::lock_guard lock(file_evidence_mutex);
  require(!file_evidence.empty(), "observed a real regular-file open");
  return file_evidence.size() - 1;
}
}  // namespace

extern "C" {
int __real_pthread_sigmask(int, const sigset_t *, sigset_t *);

int __wrap_pthread_sigmask(int how, const sigset_t *set, sigset_t *old) {
  if (mask_injection && how == SIG_BLOCK && set &&
      ::sigismember(set, SIGPIPE) == 1) {
    ++mask_injection_consumed;
    return std::exchange(mask_injection, 0);
  }
  return __real_pthread_sigmask(how, set, old);
}

int __real_openat(int, const char *, int, ...);
int __real_fstat(int, struct stat *);
ssize_t __real_read(int, void *, std::size_t);
ssize_t __real_pread(int, void *, std::size_t, off_t);
ssize_t __real_sendfile(int, int, off_t *, std::size_t);

int __wrap_openat(int directory, const char *name, int flags, ...) {
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list arguments;
    va_start(arguments, flags);
    mode = va_arg(arguments, mode_t);
    va_end(arguments);
  }
  const int fd = __real_openat(directory, name, flags, mode);
  const int saved = errno;

  struct stat info {};

  if (fd >= 0 && __real_fstat(fd, &info) == 0 && S_ISREG(info.st_mode)) {
    std::lock_guard lock(file_evidence_mutex);
    active_files.emplace(fd, file_evidence.size());
    file_evidence.push_back({fd, info.st_dev, info.st_ino});
  }
  errno = saved;
  return fd;
}

int __wrap_fstat(int fd, struct stat *info) {
  {
    std::lock_guard lock(file_evidence_mutex);
    if (auto item = active_files.find(fd); item != active_files.end())
      ++file_evidence[item->second].stats;
  }
  return __real_fstat(fd, info);
}

int __wrap_close(int fd) {
  {
    std::lock_guard lock(file_evidence_mutex);
    if (auto item = active_files.find(fd); item != active_files.end()) {
      ++file_evidence[item->second].closes;
      active_files.erase(item);
    }
  }
  return __wrap_socket_close(fd);
}

ssize_t __wrap_read(int fd, void *bytes, std::size_t count) {
  {
    std::lock_guard lock(file_evidence_mutex);
    if (auto item = active_files.find(fd); item != active_files.end())
      ++file_evidence[item->second].reads;
  }
  return __real_read(fd, bytes, count);
}

ssize_t __wrap_pread(int fd, void *bytes, std::size_t count, off_t offset) {
  {
    std::lock_guard lock(file_evidence_mutex);
    if (auto item = active_files.find(fd); item != active_files.end())
      ++file_evidence[item->second].preads;
  }
  return __real_pread(fd, bytes, count, offset);
}

ssize_t __wrap_sendfile(int output, int input, off_t *offset,
                        std::size_t count) {
  const off_t before = offset ? *offset : -1;
  const int injected = sendfile_injection;
  if (sendfile_injection_repeats > 1)
    --sendfile_injection_repeats;
  else
    sendfile_injection = 0;
  ssize_t result;
  if (injected) {
    ++sendfile_injection_consumed;
    errno = injected;
    result = -1;
  } else {
    result = __real_sendfile(output, input, offset, count);
  }
  const int saved = errno;
  {
    std::lock_guard lock(file_evidence_mutex);
    if (auto item = active_files.find(input); item != active_files.end()) {
      auto &record = file_evidence[item->second];
      if (injected) {
        ++record.injected;
      } else {
        ++record.calls;
        if (result > 0) {
          ++record.positive;
          record.bytes += static_cast<std::size_t>(result);
          record.short_writes += static_cast<std::size_t>(result) < count;
        }
        record.eagain +=
            result < 0 && (saved == EAGAIN || saved == EWOULDBLOCK);
      }
      record.offset_error |=
          !offset || *offset != before + (result > 0 ? result : 0);
    }
  }
  errno = saved;
  return result;
}
}

namespace {
Socket open_fixture(const Fixture &fixture) {
  const auto path = fixture.root / "large.bin";
  const int fd = ::openat(AT_FDCWD, path.c_str(), O_RDONLY | O_CLOEXEC);
  require(fd >= 0, "open stable fixture file");
  return Socket(fd);
}

void raw_observation(const Fixture &fixture, int disabled_error) {
  int pair[2];
  require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0,
          "observer pair");
  Socket output(pair[0]), input(pair[1]);
  auto file = open_fixture(fixture);
  const auto id = latest_file();
  off_t offset = 0;
  require(::sendfile(output.fd(), file.fd(), &offset, 1024) == 1024,
          "real sendfile observer");
  std::array<char, 1024> bytes;
  require(::recv(input.fd(), bytes.data(), bytes.size(), 0) == 1024,
          "raw observer receive");
  require(std::memcmp(bytes.data(), fixture.large.data(), bytes.size()) == 0,
          "raw bytes exact");
  for (int error : {EINTR, EIO, ENOSYS}) {
    const auto before = sendfile_injection_consumed;
    sendfile_injection = disabled_error == error ? 0 : error;
    errno = 0;
    const auto result = ::sendfile(output.fd(), file.fd(), &offset, 1);
    const auto actual = errno;
    require(result == -1 && actual == error &&
                sendfile_injection_consumed == before + 1,
            "sendfile injection must be consumed");
  }
  const auto record = evidence(id);
  require(record.positive == 1 && record.bytes == 1024 &&
              !record.offset_error && record.reads == 0 && record.preads == 0 &&
              record.injected == 3,
          "observer distinguishes real syscall from injection");
  std::cout << "M0 observer real_calls=" << record.calls
            << " bytes=" << record.bytes << " injected=" << record.injected
            << " read/pread=0/0 offset=" << offset << '\n';
}

void default_sigpipe_fixture(const Fixture &fixture) {
  const pid_t child = ::fork();
  require(child >= 0, "SIGPIPE fixture fork");
  if (child == 0) {
    struct sigaction action {};

    action.sa_handler = SIG_DFL;
    ::sigemptyset(&action.sa_mask);
    if (::sigaction(SIGPIPE, &action, nullptr) != 0) ::_exit(90);
    sigset_t signal;
    ::sigemptyset(&signal);
    ::sigaddset(&signal, SIGPIPE);
    if (::pthread_sigmask(SIG_UNBLOCK, &signal, nullptr) != 0) ::_exit(91);
    int pair[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) ::_exit(92);
    ::close(pair[1]);
    auto file = open_fixture(fixture);
    off_t offset = 0;
    ::sendfile(pair[0], file.fd(), &offset, 1);
    ::_exit(93);
  }
  int status = 0;
  require(::waitpid(child, &status, 0) == child, "SIGPIPE fixture reap");
  require(WIFSIGNALED(status) && WTERMSIG(status) == SIGPIPE,
          "unguarded sendfile negative control dies by default SIGPIPE");
  std::cout << "M0 default_SIGPIPE negative child signal=" << WTERMSIG(status)
            << '\n';
}

void fixture_owner(const Fixture &fixture) {
  const auto baseline = mask_lifetime();
  hp::http::StaticFileService service(fixture.root.string());
  ServerHarness harness(service, 2);
  const auto owned = ready_threads(harness);
  Stream client(harness.port);
  client.send(query("/note.txt", true));
  response(client.next(), 200, "hello from S3\n");
  client.eof();
  harness.server->request_graceful_shutdown(Clock::now() + 1s);
  join_graceful(harness);
  settle_threads(owned, baseline, "sendfile-M0", 0);
}
}  // namespace

namespace hp::net {
struct ConnectionIoTestAccess {
  static std::size_t input_capacity(const ConnectionIo &io) {
    return io.input_.capacity();
  }

  static std::size_t capacity(const ConnectionIo &io) {
    return io.output_.capacity();
  }
};
}  // namespace hp::net

namespace {
using hp::base::FileRegion;
using hp::base::UniqueFd;
static_assert(!std::is_copy_constructible_v<FileRegion> &&
              !std::is_copy_assignable_v<FileRegion>);
static_assert(std::is_nothrow_move_constructible_v<FileRegion> &&
              std::is_nothrow_move_assignable_v<FileRegion>);

FileRegion region(const Fixture &fixture, std::size_t length,
                  off_t offset = 0) {
  auto file = open_fixture(fixture);
  return FileRegion(UniqueFd(file.release()), offset, length);
}

std::span<const std::byte> view(std::string_view text) {
  return {reinterpret_cast<const std::byte *>(text.data()), text.size()};
}

void collect(int fd, std::string &target) {
  std::array<char, 65536> buffer;
  for (;;) {
    const auto count = ::recv(fd, buffer.data(), buffer.size(), MSG_DONTWAIT);
    if (count > 0) {
      target.append(buffer.data(), static_cast<std::size_t>(count));
      continue;
    }
    require(count == 0 || errno == EAGAIN || errno == EWOULDBLOCK,
            "collect file output");
    return;
  }
}

void call_budget_and_mask_failure(const Fixture &fixture, bool disable_budget,
                                  bool disable_mask) {
  int pair[2];
  require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0,
          "budget pair");
  Socket peer(pair[1]);
  int large = 1024 * 1024;
  ::setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &large, sizeof(large));
  ConnectionIo io{Socket(pair[0])};
  io.queue_file({}, region(fixture, 1024 * 1024));
  const auto id = latest_file();
  const auto before = sendfile_injection_consumed;
  sendfile_injection = disable_budget ? 0 : EINTR;
  sendfile_injection_repeats = 100;
  const auto interrupted = io.write_available();
  sendfile_injection = 0;
  sendfile_injection_repeats = 0;
  require(interrupted.bytes_written == 0 && interrupted.error_number == 0 &&
              sendfile_injection_consumed - before ==
                  ConnectionIo::file_call_budget &&
              io.pending_bytes() == 1024 * 1024,
          "EINTR call budget must stop at sixteen");
  sigset_t original, restored;
  ::pthread_sigmask(SIG_SETMASK, nullptr, &original);
  const auto mask_before = mask_injection_consumed;
  mask_injection = disable_mask ? 0 : EIO;
  const auto rejected = io.write_available();
  require(rejected.error_number == EIO && evidence(id).calls == 0 &&
              mask_injection_consumed == mask_before + 1,
          "mask failure must prevent unprotected sendfile");
  ::pthread_sigmask(SIG_SETMASK, nullptr, &restored);
  for (int signal = 1; signal < NSIG; ++signal)
    require(
        ::sigismember(&original, signal) == ::sigismember(&restored, signal),
        "mask failure preserves full mask");
  const auto progress = io.write_available();
  require(progress.error_number == 0 &&
              progress.bytes_written == ConnectionIo::file_write_budget &&
              evidence(id).bytes == ConnectionIo::file_write_budget,
          "real writable socket reaches exact file byte budget then yields");
  std::cout << "M1 call_budget=" << ConnectionIo::file_call_budget
            << " real_byte_budget=" << progress.bytes_written
            << " guard_failure_calls=0\n";
}

void file_registration_failure(const Fixture &fixture, bool disabled) {
  EventLoop loop;
  int pair[2];
  require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0,
          "file registration pair");
  Socket peer(pair[1]);
  std::size_t id = 0;
  bool failed = false;
  const auto before = registration_injections;
  {
    TcpConnection connection(loop, Socket(pair[0]), 1, {}, 0, [](int, auto) {});
    connection.send_file(view("header"), region(fixture, 1024));
    id = latest_file();
    reject_any_registration = !disabled;
    try {
      connection.start();
    } catch (const std::system_error &error) {
      require(error.code() == std::error_code(EIO, std::generic_category()),
              "file registration exact EIO");
      failed = true;
    }
  }
  require(failed && registration_injections == before + 1 &&
              evidence(id).closes == 1,
          "file registration injection must fail and close region");
  std::cout << "M1 staged_file registration_EIO=1 close=1\n";
}

void transport_bounds(const Fixture &fixture, bool disable_allocation) {
  auto initial = region(fixture, 7, 3);
  const auto moved_id = latest_file();
  auto moved = std::move(initial);
  require(initial.fd() == -1 && initial.remaining() == 0 && moved.offset() == 3,
          "region move transfers sole ownership");
  auto replacement = region(fixture, 1);
  const auto replaced_id = latest_file();
  replacement = std::move(moved);
  require(evidence(replaced_id).closes == 1 && moved.fd() == -1,
          "move assignment releases old file once");
  require((::fcntl(replacement.fd(), F_GETFD) & FD_CLOEXEC) != 0,
          "file CLOEXEC");
  replacement.advance(7);
  require(evidence(moved_id).closes == 1,
          "region completes and releases file immediately");
  for (const auto length :
       {ConnectionIo::output_limit - 2, ConnectionIo::output_limit - 1,
        ConnectionIo::output_limit}) {
    int pair[2];
    require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0,
            "bounds pair");
    Socket peer(pair[1]);
    ConnectionIo io{Socket(pair[0])};
    auto file = region(fixture, length);
    const auto id = latest_file();
    bool rejected = false;
    try {
      io.queue_file(view("H"), std::move(file));
    } catch (const std::length_error &) {
      rejected = true;
    }
    require(rejected == (length == ConnectionIo::output_limit),
            "header plus file limit +/-1");
    if (rejected) {
      require(io.pending_bytes() == 0 && evidence(id).closes == 1,
              "rejected region closes with no partial submission");
    } else {
      require(io.pending_bytes() == length + 1 &&
                  ConnectionIoTestAccess::capacity(io) < 1024,
              "logical file bytes do not allocate body vector");
      bool append_rejected = false;
      try {
        io.queue_output(view("late"));
      } catch (const std::logic_error &) {
        append_rejected = true;
      }
      require(append_rejected && io.pending_bytes() == length + 1,
              "append cannot reorder a pending file");
    }
  }
  require(!ConnectionIo::output_fits(1, SIZE_MAX) &&
              !ConnectionIo::output_fits(SIZE_MAX, 1),
          "overflow-safe file sum predicate");
  int pair[2];
  require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0,
          "allocation pair");
  Socket peer(pair[1]);
  ConnectionIo io{Socket(pair[0])};
  auto file = region(fixture, 1);
  const auto id = latest_file();
  bool failed = false;
  timer_allocation_failure = disable_allocation ? -1 : 0;
  try {
    io.queue_file(view("header"), std::move(file));
  } catch (const std::bad_alloc &) {
    failed = true;
  }
  timer_allocation_failure = -1;
  require(
      failed && io.pending_bytes() == 0 && evidence(id).closes == 1,
      "submission allocation failure preserves empty output and closes region");
  std::cout << "M1 bounds limit=" << ConnectionIo::output_limit
            << " region_move=1 allocation_rollback=1 body_vector=0\n";
}

void transport_short_writes(const Fixture &fixture) {
  int pair[2];
  require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0,
          "short write pair");
  Socket peer(pair[1]);
  int size = 4096;
  require(
      ::setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0,
      "small send buffer");
  ConnectionIo io{Socket(pair[0])};
  constexpr std::size_t length = 1024 * 1024;
  const std::string header(32768, 'H');
  io.queue_file(view(header), region(fixture, length, 17));
  const auto id = latest_file();
  const auto first = io.write_available();
  require(first.would_block && first.bytes_written > 0 &&
              first.bytes_written < header.size() && evidence(id).calls == 0,
          "real header short write precedes any sendfile");
  std::string wire;
  const auto injected_before = sendfile_injection_consumed;
  sendfile_injection = EINTR;
  const auto deadline = Clock::now() + 3s;
  std::size_t turns = 0;
  while (io.has_pending_output()) {
    collect(peer.fd(), wire);
    const auto before = evidence(id).bytes;
    const auto written = io.write_available();
    require(written.error_number == 0, "short-write transport succeeds");
    require(evidence(id).bytes - before <= ConnectionIo::file_write_budget,
            "per-turn file progress budget");
    require(Clock::now() < deadline, "short-write complete deadline");
    ++turns;
  }
  collect(peer.fd(), wire);
  const auto record = evidence(id);
  require(wire.size() == header.size() + length && wire.starts_with(header),
          "header file ordering");
  require(std::memcmp(wire.data() + header.size(), fixture.large.data() + 17,
                      length) == 0,
          "nonzero offset file exact bytes");
  require(record.positive > 0 && record.short_writes > 0 && record.eagain > 0 &&
              record.bytes == length && !record.offset_error &&
              record.closes == 1 &&
              sendfile_injection_consumed == injected_before + 1,
          "real file EAGAIN/short writes and EINTR preserve offset");
  std::cout << "M1 short header=" << first.bytes_written
            << " file_bytes=" << record.bytes << " positive=" << record.positive
            << " short=" << record.short_writes << " EAGAIN=" << record.eagain
            << " turns=" << turns
            << " capacity=" << ConnectionIoTestAccess::capacity(io) << '\n';
}

void transport_errors(const Fixture &fixture, bool disable_eof) {
  for (const auto error : {0, EIO, ENOSYS}) {
    EventLoop loop;
    ConnectionRegistry registry(loop, 0);
    int pair[2];
    require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0,
            "error pair");
    Socket peer(pair[1]);
    registry.add(
        Socket(pair[0]), [&](TcpConnection &connection, auto bytes, bool) {
          auto file = region(fixture, error ? 32 : 1,
                             (error || disable_eof) ? 0 : fixture.large.size());
          connection.consume(bytes.size());
          connection.send_file({}, std::move(file));
          sendfile_injection = error;
        });
    require(::send(peer.fd(), "x", 1, MSG_NOSIGNAL) == 1, "trigger file error");
    const auto before = sendfile_injection_consumed;
    loop.poll_once(0);
    const auto id = latest_file();
    require(ShutdownAccess::entries(registry).empty() &&
                evidence(id).closes == 1 &&
                sendfile_injection_consumed == before + (error ? 1 : 0),
            "early EOF or explicit file error closes region and connection");
    char byte;
    require(::recv(peer.fd(), &byte, 1, 0) == 0, "no appended error response");
    require(!loop.failed(), "file error is connection-local");
  }
  std::cout
      << "M1 errors early_EOF=1 EIO=1 unsupported=1 no_extra_response=1\n";
}

void transport_fairness(const Fixture &fixture) {
  EventLoop loop;
  ConnectionRegistry registry(loop, 0);
  int pair[2], healthy[2];
  require(
      ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0 &&
          ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, healthy) == 0,
      "fairness pairs");
  Socket peer(pair[1]), other(healthy[1]);
  int completed = 0, healthy_calls = 0;
  registry.add(
      Socket(pair[0]), [&](TcpConnection &connection, auto bytes, bool) {
        connection.consume(bytes.size());
        connection.set_write_complete_callback([&](TcpConnection &current) {
          ++completed;
          if (completed < 100) current.send_file({}, region(fixture, 1));
        });
        connection.send_file({}, region(fixture, 1));
      });
  registry.add(Socket(healthy[0]),
               [&](TcpConnection &connection, auto bytes, bool) {
                 ++healthy_calls;
                 connection.send(bytes);
                 connection.consume(bytes.size());
               });
  require(::send(peer.fd(), "x", 1, MSG_NOSIGNAL) == 1 &&
              ::send(other.fd(), "y", 1, MSG_NOSIGNAL) == 1,
          "fairness ready sockets");
  loop.set_control_callback(
      [&](EventLoop::Control, auto) { registry.begin_drain(true); });
  bool task = false;
  require(loop.queue_in_loop([&] {
    require(completed == 1 && healthy_calls == 1,
            "outer write loop does not bypass file budget");
    task = true;
    loop.request_force();
  }),
          "fairness task accepted");
  loop.poll_once(0);
  require(task && completed == 1 && ShutdownAccess::entries(registry).empty(),
          "owner task and force run before reentrant file suffix");
  std::cout << "M1 fairness completed_before_control=" << completed
            << " healthy=" << healthy_calls << " task=" << task << '\n';
}

void guarded_sigpipe(const Fixture &fixture, bool disable_peer_close) {
  const pid_t child = ::fork();
  require(child >= 0, "protected SIGPIPE fork");
  if (child == 0) {
    try {
      struct sigaction action {};

      action.sa_handler = SIG_DFL;
      ::sigemptyset(&action.sa_mask);
      require(::sigaction(SIGPIPE, &action, nullptr) == 0,
              "default SIGPIPE disposition");
      sigset_t pipe, original, actual, pending;
      ::sigemptyset(&pipe);
      ::sigaddset(&pipe, SIGPIPE);
      ::pthread_sigmask(SIG_UNBLOCK, &pipe, &original);
      for (int mode = 0; mode < 3; ++mode) {
        if (mode) ::pthread_sigmask(SIG_BLOCK, &pipe, nullptr);
        if (mode == 2)
          require(::pthread_kill(::pthread_self(), SIGPIPE) == 0,
                  "preexisting pending SIGPIPE");
        int pair[2];
        require(
            ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0,
            "pipe pair");
        Socket disconnected(pair[1]);
        if (!disable_peer_close) disconnected.reset();
        sigset_t saved_mask;
        ::pthread_sigmask(SIG_SETMASK, nullptr, &saved_mask);
        ConnectionIo io{Socket(pair[0])};
        io.queue_file({}, region(fixture, 1));
        const auto result = io.write_available();
        require(result.error_number == EPIPE,
                "real sendfile EPIPE returned safely");
        ::pthread_sigmask(SIG_SETMASK, nullptr, &actual);
        ::sigpending(&pending);
        for (int signal = 1; signal < NSIG; ++signal)
          require(::sigismember(&actual, signal) ==
                      ::sigismember(&saved_mask, signal),
                  "guard full mask restored");
        require(::sigismember(&actual, SIGPIPE) == (mode ? 1 : 0) &&
                    ::sigismember(&pending, SIGPIPE) == (mode ? 1 : 0),
                "original blocked and pending SIGPIPE preserved");
        if (mode) {
          const timespec zero{};
          require(::sigtimedwait(&pipe, nullptr, &zero) == SIGPIPE,
                  "consume test-owned pending");
          ::pthread_sigmask(SIG_UNBLOCK, &pipe, nullptr);
        }
      }
      {
        auto listener = Socket::create_tcp();
        listener.bind_any(0);
        listener.listen(4);
        Socket reset_peer(connect_client(listener.local_port()));
        Socket accepted;
        await([&] {
          accepted = listener.accept_non_blocking();
          return accepted.valid();
        });
        const linger reset{1, 0};
        require(::setsockopt(reset_peer.fd(), SOL_SOCKET, SO_LINGER, &reset,
                             sizeof(reset)) == 0,
                "TCP reset peer configuration");
        reset_peer.reset();
        pollfd event{accepted.fd(), POLLERR, 0};
        const int polled = ::poll(&event, 1, 1000);
        std::cerr << "TCP reset diagnostic fd=" << event.fd
                  << " poll=" << polled << " revents=" << event.revents
                  << " errno=" << errno << std::endl;
        require(polled == 1 && (event.revents & POLLERR), "TCP reset observed");
        ConnectionIo reset_io(std::move(accepted));
        reset_io.queue_file({}, region(fixture, 1));
        const auto reset_result = reset_io.write_available();
        require(reset_result.error_number == ECONNRESET ||
                    reset_result.error_number == EPIPE,
                "real TCP reset is connection-local");
        require(reset_io.write_available().error_number == EPIPE,
                "TCP reset followed by safe EPIPE");
      }
      int pair[2];
      require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0,
              "healthy pipe pair");
      Socket peer(pair[1]);
      ConnectionIo healthy{Socket(pair[0])};
      healthy.queue_file({}, region(fixture, 1));
      require(healthy.write_available().bytes_written == 1,
              "next connection survives SIGPIPE");
      ::sigaction(SIGPIPE, nullptr, &action);
      require(action.sa_handler == SIG_DFL,
              "transport did not globally ignore SIGPIPE");
      ::pthread_sigmask(SIG_SETMASK, &original, nullptr);
      ::_exit(0);
    } catch (const std::exception &error) {
      std::cerr << "SIGPIPE child FAIL: " << error.what() << std::endl;
      ::_exit(94);
    }
  }
  int status;
  require(::waitpid(child, &status, 0) == child, "protected SIGPIPE reap");
  std::cout << "SIGPIPE child exit="
            << (WIFEXITED(status) ? WEXITSTATUS(status) : -1)
            << " signal=" << (WIFSIGNALED(status) ? WTERMSIG(status) : 0)
            << std::endl;
  require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "guarded default-SIGPIPE process survives");
  std::cout << "M1 SIGPIPE default_survival=1 blocked=preserved "
               "pending=preserved TCP_RST=1 "
               "next_healthy=1\n";
}
}  // namespace

namespace hp::net {
struct SendfileTestAccess {
  static ConnectionRegistry &registry(TcpServer &server, std::size_t index) {
    return server.worker_count_ ? *server.registries_[index]
                                : *server.main_registry_;
  }

  static std::size_t capacity(const TcpConnection &connection) {
    return ConnectionIoTestAccess::capacity(connection.io_);
  }

  static std::size_t input_capacity(const TcpConnection &connection) {
    return ConnectionIoTestAccess::input_capacity(connection.io_);
  }

  static bool paused(const TcpConnection &connection) {
    return connection.read_paused_;
  }

  static auto progress(const TcpConnection &connection) {
    return connection.last_progress_;
  }

  static bool waiting(const TcpConnection &connection) {
    return connection.wait_since_.has_value();
  }

  static void write(TcpConnection &connection) {
    connection.handle_event(EPOLLOUT);
  }
};
}  // namespace hp::net

namespace {
std::size_t file_count() {
  std::lock_guard lock(file_evidence_mutex);
  return file_evidence.size();
}

std::size_t owner_capacity(ServerHarness &harness, std::size_t workers,
                           std::size_t owner) {
  std::promise<std::size_t> answer;
  auto result = answer.get_future();
  auto observe = [&] {
    auto &entries = ShutdownAccess::entries(
        SendfileTestAccess::registry(*harness.server, owner));
    require(entries.size() == 1,
            "one production connection per measured owner");
    answer.set_value(SendfileTestAccess::capacity(*entries.begin()->second));
  };
  if (workers)
    require(TcpServerTestAccess::post(*harness.server, owner,
                                      [&](EventLoop &) { observe(); }),
            "capacity owner observer");
  else
    require(TcpServerTestAccess::main_post(*harness.server, observe),
            "capacity main observer");
  require(result.wait_for(3s) == std::future_status::ready,
          "capacity observer ready");
  return result.get();
}

std::string materialize_prepared(hp::http::ResponseResult prepared) {
  if (!prepared.file)
    return {reinterpret_cast<const char *>(prepared.bytes.data()),
            prepared.bytes.size()};
  int pair[2];
  require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0,
          "prepare comparison pair");
  Socket peer(pair[1]);
  ConnectionIo io{Socket(pair[0])};
  io.queue_file(prepared.bytes, std::move(*prepared.file));
  std::string wire;
  const auto deadline = Clock::now() + 3s;
  while (io.has_pending_output()) {
    const auto result = io.write_available();
    require(result.error_number == 0, "prepared file transfer");
    collect(peer.fd(), wire);
    require(Clock::now() < deadline, "prepared file transfer deadline");
  }
  collect(peer.fd(), wire);
  return wire;
}

void prepare_compatibility(const Fixture &fixture,
                           const hp::http::StaticFileService &service) {
  Fixture::write_text(fixture.root / "empty.bin", "");
  Fixture::write_text(fixture.root / "one.bin", "X");
  Fixture::write_bytes(fixture.root / "kilo.bin",
                       std::vector<std::byte>(fixture.large.begin(),
                                              fixture.large.begin() + 1024));
  Fixture::write_bytes(
      fixture.root / "mega.bin",
      std::vector<std::byte>(fixture.large.begin(),
                             fixture.large.begin() + 1024 * 1024));
  for (auto path : {"/empty.bin", "/one.bin", "/large.bin", "/oversized.bin",
                    "/escape.txt", "/../sibling-secret.txt", "/assets",
                    "/missing", "/assets/binary.png"}) {
    const auto before = file_count();
    auto prepared = service.prepare_response(
        {"GET", path}, hp::http::ConnectionPolicy::keep_alive);
    const auto after_prepare = file_count();
    if (prepared.file) {
      require(prepared.file->offset() == 0 && prepared.bytes.size() < 256,
              "prepared metadata and small header");
      require(evidence(after_prepare - 1).reads == 0 &&
                  evidence(after_prepare - 1).preads == 0,
              "prepare never reads body");
    }
    const auto actual = materialize_prepared(std::move(prepared));
    const auto legacy = service.handle_response(
        {"GET", path}, hp::http::ConnectionPolicy::keep_alive);
    require(
        actual.size() == legacy.bytes.size() &&
            std::memcmp(actual.data(), legacy.bytes.data(), actual.size()) == 0,
        "legacy and prepared HTTP bytes match");
    for (auto id = before; id < file_count(); ++id)
      require(evidence(id).closes == 1,
              "prepare/compatibility close every file identity");
  }
  std::cout << "M2 prepare compatibility paths=9 "
               "zero/one/8MiB/overlimit/security byte_exact=1\n";
}

void changing_files(const Fixture &fixture,
                    const hp::http::StaticFileService &service,
                    bool disable_truncate) {
  const auto path = fixture.root / "replace.txt";
  const auto next = fixture.root / "replacement.txt";
  Fixture::write_text(path, "old");
  auto opened = service.prepare_response({"GET", "/replace.txt"});
  const auto old_id = latest_file();
  Fixture::write_text(next, "new contents");
  std::filesystem::rename(next, path);
  require(materialize_prepared(std::move(opened)).ends_with("\r\n\r\nold"),
          "rename keeps opened inode");
  auto replaced = service.prepare_response({"GET", "/replace.txt"});
  const auto new_id = latest_file();
  require(evidence(old_id).inode != evidence(new_id).inode &&
              materialize_prepared(std::move(replaced))
                  .ends_with("\r\n\r\nnew contents"),
          "next request opens replacement inode");
  Fixture::write_text(path, "grow");
  auto growing = service.prepare_response({"GET", "/replace.txt"});
  Fixture::write_text(path, "grow plus appended data");
  require(materialize_prepared(std::move(growing)).ends_with("\r\n\r\ngrow"),
          "growth cannot exceed original Content-Length");
  Fixture::write_text(path, "truncate original");
  auto truncated = service.prepare_response({"GET", "/replace.txt"});
  const auto truncated_id = latest_file();
  if (!disable_truncate) Fixture::write_text(path, "");
  EventLoop loop;
  ConnectionRegistry registry(loop, 0);
  int pair[2];
  require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0,
          "truncate pair");
  Socket peer(pair[1]);
  registry.add(
      Socket(pair[0]), [&](TcpConnection &connection, auto input, bool) {
        connection.consume(input.size());
        connection.send_file(truncated.bytes, std::move(*truncated.file));
      });
  require(::send(peer.fd(), "x", 1, MSG_NOSIGNAL) == 1, "truncate trigger");
  loop.poll_once(0);
  std::string wire;
  collect(peer.fd(), wire);
  require(ShutdownAccess::entries(registry).empty() &&
              evidence(truncated_id).closes == 1 &&
              wire.starts_with("HTTP/1.1 200") && wire.ends_with("\r\n\r\n") &&
              wire.find("500") == std::string::npos,
          "truncate closes without extra error response");
  std::cout << "M2 file identity rename=old/new growth=initial_length "
               "truncate=header_only_EOF\n";
}

void production_files(const Fixture &fixture,
                      const hp::http::StaticFileService &service) {
  for (std::size_t workers : {0U, 1U, 2U}) {
    const auto begin = file_count();
    ServerHarness harness(service, workers);
    std::size_t request_index = 0;
    for (const auto &[path, length] :
         std::vector<std::pair<std::string, std::size_t>>{
             {"/kilo.bin", 1024},
             {"/mega.bin", 1024 * 1024},
             {"/large.bin", fixture.large.size()}}) {
      Stream client(harness.port);
      const auto opening = file_count();
      client.send(query(path));
      await([&] {
        if (file_count() <= opening) return false;
        const auto record = evidence(opening);
        return length == fixture.large.size() ? record.eagain > 0
                                              : record.positive > 0;
      });
      const auto owner = workers ? request_index % workers : 0;
      const auto capacity = owner_capacity(harness, workers, owner);
      const auto received = client.next();
      require(received.status == 200 && received.body.size() == length &&
                  std::memcmp(received.body.data(), fixture.large.data(),
                              length) == 0,
              "production size ladder byte exact");
      require(capacity < 256 && received.header.size() < 256,
              "production memory independent of body size");
      client.send(query("/missing", true));
      response(client.next(), 404, "404 Not Found\n");
      client.eof();
      const auto record = evidence(opening);
      require(record.reads == 0 && record.preads == 0 &&
                  record.bytes == length && record.closes == 1 &&
                  !record.offset_error,
              "production sendfile without read/pread fallback");
      std::cout << "M2 production workers=" << workers << " length=" << length
                << " sendfile_bytes=" << record.bytes
                << " positive=" << record.positive
                << " EAGAIN=" << record.eagain
                << " header=" << received.header.size()
                << " capacity=" << capacity << " read/pread=0/0\n";
      ++request_index;
    }
    Stream pipeline(harness.port);
    pipeline.send(query("/note.txt") + query("/one.bin"));
    require(::shutdown(pipeline.fd, SHUT_WR) == 0, "pipeline peer FIN");
    response(pipeline.next(), 200, "hello from S3\n");
    response(pipeline.next(), 200, "X");
    pipeline.eof();
    harness.stop();
    for (auto id = begin; id < file_count(); ++id)
      require(evidence(id).closes == 1 && evidence(id).reads == 0 &&
                  evidence(id).preads == 0,
              "production each opened region closes exactly once");
  }
}

void file_progress(const Fixture &fixture,
                   const hp::http::StaticFileService &service) {
  EventLoop loop;
  ConnectionRegistry registry(loop, hp::http::max_request_bytes,
                              {150ms, 100ms});
  int pair[2];
  require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0,
          "progress pair");
  Socket peer(pair[1]);
  int small = 4096;
  ::setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
  int providers = 0;
  hp::app::HttpCallbackStats stats;
  registry.add(Socket(pair[0]), hp::app::make_http_callback(
                                    [&](const auto &request, auto policy) {
                                      ++providers;
                                      return service.prepare_response(request,
                                                                      policy);
                                    },
                                    &stats));
  auto &connection = *ShutdownAccess::entries(registry).at(pair[0]);
  const auto request = query("/large.bin") + query("/note.txt");
  require(::send(peer.fd(), request.data(), request.size(), MSG_NOSIGNAL) ==
              static_cast<ssize_t>(request.size()),
          "file pipeline trigger");
  loop.poll_once(0);
  const auto id = latest_file();
  auto previous = SendfileTestAccess::progress(connection);
  const auto queued = connection.pending_bytes();
  SendfileTestAccess::write(connection);
  require(SendfileTestAccess::progress(connection) == previous &&
              connection.pending_bytes() == queued &&
              !SendfileTestAccess::waiting(connection) && providers == 1 &&
              stats.responses == 1,
          "file EAGAIN neither renews idle nor completes pipeline");
  std::string received;
  const auto began = Clock::now();
  for (int cycle = 0; cycle < 10; ++cycle) {
    std::this_thread::sleep_for(20ms);
    collect(peer.fd(), received);
    SendfileTestAccess::write(connection);
    require(SendfileTestAccess::progress(connection) > previous &&
                connection.pending_bytes() > 0,
            "actual sendfile progress refreshes idle");
    previous = SendfileTestAccess::progress(connection);
  }
  require(Clock::now() - began >= 200ms && providers == 1,
          "progress keeps transfer beyond idle duration");
  while (!ShutdownAccess::entries(registry).empty()) loop.poll_once(200);
  require(loop.timer_count() == 0 && evidence(id).closes == 1 && providers == 1,
          "stalled file idle closes region without pipeline suffix");
  std::cout << "M2 file_progress bytes=" << evidence(id).bytes
            << " refreshed_cycles=10" << " EAGAIN=" << evidence(id).eagain
            << " provider=" << providers << " timer=0 idle_closed=1\n";
  (void)fixture;
}

void unfinished_lifecycle(const hp::http::StaticFileService &service) {
  const auto baseline = settle_threads({}, {}, "file-baseline", -1);
  const auto fds = resources("/proc/self/fd");
  const auto files = file_count();
  const auto sockets = accepted_sockets.load();
  for (int cycle = 0; cycle < 100; ++cycle) {
    ServerHarness harness(service, 2);
    const auto owned = ready_threads(harness);
    Stream first(harness.port), second(harness.port);
    first.send(query("/large.bin"));
    second.send(query("/large.bin"));
    await([&] {
      return pending(harness, 2, 0) > 0 && pending(harness, 2, 1) > 0;
    });
    if (cycle == 99) {
      require(TcpServerTestAccess::post(*harness.server, 0,
                                        [](EventLoop &) {
                                          throw std::runtime_error(
                                              "file fatal original");
                                        }),
              "file fatal accepted");
      harness.failed("file fatal original");
    } else {
      harness.server->request_graceful_shutdown(Clock::now());
      join_graceful(harness);
    }
    settle_threads(owned, baseline, "file-joined", cycle);
  }
  require(file_count() - files == 200 && accepted_sockets - sockets == 200,
          "100 rounds hold two unfinished file responses");
  for (auto id = files; id < file_count(); ++id)
    require(evidence(id).closes == 1 &&
                evidence(id).bytes < hp::http::max_file_bytes,
            "unfinished file identity closed exactly once");
  require(resources("/proc/self/fd") == fds && task_ids() == baseline,
          "unfinished file lifecycle fd and exact TID baseline");
  std::cout << "M2 lifecycle cycles=100 files=200 sockets=200 fd=" << fds << '/'
            << resources("/proc/self/fd") << " fatal_original_after_join=1\n";
}
}  // namespace

#ifndef HP_SENDFILE_ENTRY
#define HP_SENDFILE_ENTRY main
#endif
int HP_SENDFILE_ENTRY(int argc, char **argv) {
  try {
    Fixture fixture;
    const auto option =
        argc == 2 ? std::string_view(argv[1]) : std::string_view{};
    int disabled_error = 0;
    if (argc == 2 && std::string_view(argv[1]) == "--no-eintr")
      disabled_error = EINTR;
    if (argc == 2 && std::string_view(argv[1]) == "--no-eio")
      disabled_error = EIO;
    if (argc == 2 && std::string_view(argv[1]) == "--no-unsupported")
      disabled_error = ENOSYS;
    raw_observation(fixture, disabled_error);
    default_sigpipe_fixture(fixture);
    transport_bounds(fixture, option == "--no-allocation");
    call_budget_and_mask_failure(fixture, option == "--no-budget-eintr",
                                 option == "--no-mask-error");
    file_registration_failure(fixture, option == "--no-file-registration");
    transport_short_writes(fixture);
    transport_errors(fixture, option == "--no-early-eof");
    transport_fairness(fixture);
    guarded_sigpipe(fixture, option == "--no-peer-close");
    fixture_owner(fixture);
    hp::http::StaticFileService service(fixture.root.string());
    watched_root = hp::http::StaticFileServiceTestAccess::root_fd(service);
    hp::app::set_session_observer_for_test(session_event);
    prepare_compatibility(fixture, service);
    changing_files(fixture, service, option == "--no-truncate");
    production_files(fixture, service);
    file_progress(fixture, service);
    library_drain(service, fixture);
    deadline_drain(service, fixture);
    unfinished_lifecycle(service);
    hp::app::set_session_observer_for_test(nullptr);
    require(root_errors == 0 && live_sessions == 0,
            "file session/service owner lifetime");
    {
      std::lock_guard lock(file_evidence_mutex);
      require(active_files.empty(), "M0 all observed file identities closed");
      for (const auto &item : file_evidence)
        require(item.closes == 1, "M0 each file identity closed once");
    }
    std::cout << "sendfile_tests M2: PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
