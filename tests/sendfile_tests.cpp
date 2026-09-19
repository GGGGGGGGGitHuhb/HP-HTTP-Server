#include <functional>
#include <type_traits>
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

FileEvidence Evidence(std::size_t index) {
  std::lock_guard lock(file_evidence_mutex);
  return file_evidence.at(index);
}

std::size_t LatestFile() {
  std::lock_guard lock(file_evidence_mutex);
  Require(!file_evidence.empty(), "observed a real regular-file open");
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

ssize_t __wrap_sendfile(int output,
                        int input,
                        off_t *offset,
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
Socket OpenFixture(const Fixture &fixture) {
  const auto path = fixture.root_ / "large.bin";
  const int fd = ::openat(AT_FDCWD, path.c_str(), O_RDONLY | O_CLOEXEC);
  Require(fd >= 0, "open stable fixture file");
  return Socket(fd);
}

void RawObservation(const Fixture &fixture, int disabled_error) {
  int pair[2];
  Require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0,
          "observer pair");
  Socket output(pair[0]), input(pair[1]);
  auto file = OpenFixture(fixture);
  const auto id = LatestFile();
  off_t offset = 0;
  Require(::sendfile(output.fd(), file.fd(), &offset, 1024) == 1024,
          "real sendfile observer");
  std::array<char, 1024> bytes;
  Require(::recv(input.fd(), bytes.data(), bytes.size(), 0) == 1024,
          "raw observer receive");
  Require(std::memcmp(bytes.data(), fixture.large_.data(), bytes.size()) == 0,
          "raw bytes exact");
  for (int error : {EINTR, EIO, ENOSYS}) {
    const auto before = sendfile_injection_consumed;
    sendfile_injection = disabled_error == error ? 0 : error;
    errno = 0;
    const auto result = ::sendfile(output.fd(), file.fd(), &offset, 1);
    const auto actual = errno;
    Require(result == -1 && actual == error &&
                sendfile_injection_consumed == before + 1,
            "sendfile injection must be consumed");
  }
  const auto record = Evidence(id);
  Require(record.positive == 1 && record.bytes == 1024 &&
              !record.offset_error && record.reads == 0 && record.preads == 0 &&
              record.injected == 3,
          "observer distinguishes real syscall from injection");
  std::cout << "M0 observer real_calls=" << record.calls
            << " bytes=" << record.bytes << " injected=" << record.injected
            << " read/pread=0/0 offset=" << offset << '\n';
}

void DefaultSigpipeFixture(const Fixture &fixture) {
  const pid_t child = ::fork();
  Require(child >= 0, "SIGPIPE fixture fork");
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
    auto file = OpenFixture(fixture);
    off_t offset = 0;
    ::sendfile(pair[0], file.fd(), &offset, 1);
    ::_exit(93);
  }
  int status = 0;
  Require(::waitpid(child, &status, 0) == child, "SIGPIPE fixture reap");
  Require(WIFSIGNALED(status) && WTERMSIG(status) == SIGPIPE,
          "unguarded sendfile negative control dies by default SIGPIPE");
  std::cout << "M0 default_SIGPIPE negative child signal=" << WTERMSIG(status)
            << '\n';
}

void FixtureOwner(const Fixture &fixture) {
  const auto baseline = MaskLifetime();
  hp::http::StaticFileService service(fixture.root_.string());
  ServerHarness harness(service, 2);
  const auto owned = ReadyThreads(harness);
  Stream client(harness.port);
  client.Send(Query("/note.txt", true));
  Response(client.ReadResponse(), 200, "hello from S3\n");
  client.ExpectEof();
  harness.server->RequestGracefulShutdown(Clock::now() + 1s);
  JoinGraceful(harness);
  SettleThreads(owned, baseline, "sendfile-M0", 0);
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

FileRegion Region(const Fixture &fixture,
                  std::size_t length,
                  off_t offset = 0) {
  auto file = OpenFixture(fixture);
  return FileRegion(UniqueFd(file.Release()), offset, length);
}

std::span<const std::byte> View(std::string_view text) {
  return {reinterpret_cast<const std::byte *>(text.data()), text.size()};
}

void Collect(int fd, std::string &target) {
  std::array<char, 65536> buffer;
  for (;;) {
    const auto count = ::recv(fd, buffer.data(), buffer.size(), MSG_DONTWAIT);
    if (count > 0) {
      target.append(buffer.data(), static_cast<std::size_t>(count));
      continue;
    }
    Require(count == 0 || errno == EAGAIN || errno == EWOULDBLOCK,
            "collect file output");
    return;
  }
}

void CallBudgetAndMaskFailure(const Fixture &fixture,
                              bool disable_budget,
                              bool disable_mask) {
  int pair[2];
  Require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0,
          "budget pair");
  Socket peer(pair[1]);
  int large = 1024 * 1024;
  ::setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &large, sizeof(large));
  ConnectionIo io{Socket(pair[0])};
  io.QueueFile({}, Region(fixture, 1024 * 1024));
  const auto id = LatestFile();
  const auto before = sendfile_injection_consumed;
  sendfile_injection = disable_budget ? 0 : EINTR;
  sendfile_injection_repeats = 100;
  const auto interrupted = io.WriteAvailable();
  sendfile_injection = 0;
  sendfile_injection_repeats = 0;
  Require(interrupted.bytes_written == 0 && interrupted.error_number == 0 &&
              sendfile_injection_consumed - before ==
                  ConnectionIo::kFileCallBudget &&
              io.pending_bytes() == 1024 * 1024,
          "EINTR call budget must stop at sixteen");
  sigset_t original, restored;
  ::pthread_sigmask(SIG_SETMASK, nullptr, &original);
  const auto mask_before = mask_injection_consumed;
  mask_injection = disable_mask ? 0 : EIO;
  const auto rejected = io.WriteAvailable();
  Require(rejected.error_number == EIO && Evidence(id).calls == 0 &&
              mask_injection_consumed == mask_before + 1,
          "mask failure must prevent unprotected sendfile");
  ::pthread_sigmask(SIG_SETMASK, nullptr, &restored);
  for (int signal = 1; signal < NSIG; ++signal)
    Require(
        ::sigismember(&original, signal) == ::sigismember(&restored, signal),
        "mask failure preserves full mask");
  const auto progress = io.WriteAvailable();
  Require(progress.error_number == 0 &&
              progress.bytes_written == ConnectionIo::kFileWriteBudget &&
              Evidence(id).bytes == ConnectionIo::kFileWriteBudget,
          "real writable socket reaches exact file byte budget then yields");
  std::cout << "M1 call_budget=" << ConnectionIo::kFileCallBudget
            << " real_byte_budget=" << progress.bytes_written
            << " guard_failure_calls=0\n";
}

void FileRegistrationFailure(const Fixture &fixture, bool disabled) {
  EventLoop loop;
  int pair[2];
  Require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0,
          "file registration pair");
  Socket peer(pair[1]);
  std::size_t id = 0;
  bool failed = false;
  const auto before = registration_injections;
  {
    TcpConnection connection(loop, Socket(pair[0]), 1, 0);
    struct OnConnectionClosedObserver1 {
      void OnConnectionClosed(int, TcpConnection::Identity) {}
    };
    connection.set_OnConnectionClosed_callback(
        std::bind_front(&OnConnectionClosedObserver1::OnConnectionClosed,
                        OnConnectionClosedObserver1{}));
    connection.SendFile(View("header"), Region(fixture, 1024));
    id = LatestFile();
    reject_any_registration = !disabled;
    try {
      connection.Start();
    } catch (const std::system_error &error) {
      Require(error.code() == std::error_code(EIO, std::generic_category()),
              "file registration exact EIO");
      failed = true;
    }
  }
  Require(failed && registration_injections == before + 1 &&
              Evidence(id).closes == 1,
          "file registration injection must fail and close region");
  std::cout << "M1 staged_file registration_EIO=1 close=1\n";
}

void TransportBounds(const Fixture &fixture, bool disable_allocation) {
  auto initial = Region(fixture, 7, 3);
  const auto moved_id = LatestFile();
  auto moved = std::move(initial);
  Require(initial.fd() == -1 && initial.remaining() == 0 && moved.offset() == 3,
          "region move transfers sole ownership");
  auto replacement = Region(fixture, 1);
  const auto replaced_id = LatestFile();
  replacement = std::move(moved);
  Require(Evidence(replaced_id).closes == 1 && moved.fd() == -1,
          "move assignment releases old file once");
  Require((::fcntl(replacement.fd(), F_GETFD) & FD_CLOEXEC) != 0,
          "file CLOEXEC");
  replacement.Advance(7);
  Require(Evidence(moved_id).closes == 1,
          "region completes and releases file immediately");
  for (const auto kLength : {ConnectionIo::kOutputLimit - 2,
                             ConnectionIo::kOutputLimit - 1,
                             ConnectionIo::kOutputLimit}) {
    int pair[2];
    Require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0,
            "bounds pair");
    Socket peer(pair[1]);
    ConnectionIo io{Socket(pair[0])};
    auto file = Region(fixture, kLength);
    const auto id = LatestFile();
    bool rejected = false;
    try {
      io.QueueFile(View("H"), std::move(file));
    } catch (const std::length_error &) {
      rejected = true;
    }
    Require(rejected == (kLength == ConnectionIo::kOutputLimit),
            "header plus file limit +/-1");
    if (rejected) {
      Require(io.pending_bytes() == 0 && Evidence(id).closes == 1,
              "rejected region closes with no partial submission");
    } else {
      Require(io.pending_bytes() == kLength + 1 &&
                  ConnectionIoTestAccess::capacity(io) < 1024,
              "logical file bytes do not allocate body vector");
      bool append_rejected = false;
      try {
        io.QueueOutput(View("late"));
      } catch (const std::logic_error &) {
        append_rejected = true;
      }
      Require(append_rejected && io.pending_bytes() == kLength + 1,
              "append cannot reorder a pending file");
    }
  }
  Require(!ConnectionIo::OutputFits(1, SIZE_MAX) &&
              !ConnectionIo::OutputFits(SIZE_MAX, 1),
          "overflow-safe file sum predicate");
  int pair[2];
  Require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0,
          "allocation pair");
  Socket peer(pair[1]);
  ConnectionIo io{Socket(pair[0])};
  auto file = Region(fixture, 1);
  const auto id = LatestFile();
  bool failed = false;
  timer_allocation_failure = disable_allocation ? -1 : 0;
  try {
    io.QueueFile(View("header"), std::move(file));
  } catch (const std::bad_alloc &) {
    failed = true;
  }
  timer_allocation_failure = -1;
  Require(
      failed && io.pending_bytes() == 0 && Evidence(id).closes == 1,
      "submission allocation failure preserves empty output and closes region");
  std::cout << "M1 bounds limit=" << ConnectionIo::kOutputLimit
            << " region_move=1 allocation_rollback=1 body_vector=0\n";
}

void TransportShortWrites(const Fixture &fixture) {
  int pair[2];
  Require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0,
          "short write pair");
  Socket peer(pair[1]);
  int size = 4096;
  Require(
      ::setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0,
      "small send buffer");
  ConnectionIo io{Socket(pair[0])};
  constexpr std::size_t kLength = 1024 * 1024;
  const std::string header(32768, 'H');
  io.QueueFile(View(header), Region(fixture, kLength, 17));
  const auto id = LatestFile();
  const auto first = io.WriteAvailable();
  Require(first.would_block && first.bytes_written > 0 &&
              first.bytes_written < header.size() && Evidence(id).calls == 0,
          "real header short write precedes any sendfile");
  std::string wire;
  const auto injected_before = sendfile_injection_consumed;
  sendfile_injection = EINTR;
  const auto deadline = Clock::now() + 3s;
  std::size_t turns = 0;
  while (io.has_pending_output()) {
    Collect(peer.fd(), wire);
    const auto before = Evidence(id).bytes;
    const auto written = io.WriteAvailable();
    Require(written.error_number == 0, "short-write transport succeeds");
    Require(Evidence(id).bytes - before <= ConnectionIo::kFileWriteBudget,
            "per-turn file progress budget");
    Require(Clock::now() < deadline, "short-write complete deadline");
    ++turns;
  }
  Collect(peer.fd(), wire);
  const auto record = Evidence(id);
  Require(wire.size() == header.size() + kLength && wire.starts_with(header),
          "header file ordering");
  Require(std::memcmp(wire.data() + header.size(),
                      fixture.large_.data() + 17,
                      kLength) == 0,
          "nonzero offset file exact bytes");
  Require(record.positive > 0 && record.short_writes > 0 && record.eagain > 0 &&
              record.bytes == kLength && !record.offset_error &&
              record.closes == 1 &&
              sendfile_injection_consumed == injected_before + 1,
          "real file EAGAIN/short writes and EINTR preserve offset");
  std::cout << "M1 short header=" << first.bytes_written
            << " file_bytes=" << record.bytes << " positive=" << record.positive
            << " short=" << record.short_writes << " EAGAIN=" << record.eagain
            << " turns=" << turns
            << " capacity=" << ConnectionIoTestAccess::capacity(io) << '\n';
}

void TransportErrors(const Fixture &fixture, bool disable_eof) {
  for (const auto error : {0, EIO, ENOSYS}) {
    EventLoop loop;
    ConnectionRegistry registry(loop, 0);
    int pair[2];
    Require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0,
            "error pair");
    Socket peer(pair[1]);

    using HandleMessageTaskFixtureState = decltype((fixture));
    using HandleMessageTaskErrorState = decltype((error));
    using HandleMessageTaskDisableEofState = decltype((disable_eof));
    struct HandleMessageTask {
      HandleMessageTaskFixtureState fixture;
      HandleMessageTaskErrorState error;
      HandleMessageTaskDisableEofState disable_eof;
      decltype(auto) HandleMessage(TcpConnection &connection,
                                   std::span<const std::byte> bytes,
                                   bool) const {
        auto file = Region(fixture,
                           error ? 32 : 1,
                           (error || disable_eof) ? 0 : fixture.large_.size());
        connection.Consume(bytes.size());
        connection.SendFile({}, std::move(file));
        sendfile_injection = error;
      }
    };
    registry.AddConnection(
        Socket(pair[0]),
        std::bind(&HandleMessageTask::HandleMessage,
                  HandleMessageTask{fixture, error, disable_eof},
                  std::placeholders::_1,
                  std::placeholders::_2,
                  std::placeholders::_3));
    Require(::send(peer.fd(), "x", 1, MSG_NOSIGNAL) == 1, "trigger file error");
    const auto before = sendfile_injection_consumed;
    loop.PollOnce(0);
    const auto id = LatestFile();
    Require(ShutdownAccess::entries(registry).empty() &&
                Evidence(id).closes == 1 &&
                sendfile_injection_consumed == before + (error ? 1 : 0),
            "early EOF or explicit file error closes region and connection");
    char byte;
    Require(::recv(peer.fd(), &byte, 1, 0) == 0, "no appended error response");
    Require(!loop.failed(), "file error is connection-local");
  }
  std::cout
      << "M1 errors early_EOF=1 EIO=1 unsupported=1 no_extra_response=1\n";
}

void TransportFairness(const Fixture &fixture) {
  EventLoop loop;
  ConnectionRegistry registry(loop, 0);
  int pair[2], healthy[2];
  Require(
      ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0 &&
          ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, healthy) == 0,
      "fairness pairs");
  Socket peer(pair[1]), other(healthy[1]);
  int completed = 0, healthy_calls = 0;
  struct FileBudgetHandler {
    const Fixture &fixture;
    int &completed;

    void HandleMessage(TcpConnection &connection,
                       std::span<const std::byte> bytes,
                       bool) {
      connection.Consume(bytes.size());
      connection.set_HandleWriteComplete_callback(
          std::bind_front(&FileBudgetHandler::HandleWriteComplete, this));
      connection.SendFile({}, Region(fixture, 1));
    }

    void HandleWriteComplete(TcpConnection &current) {
      ++completed;
      if (completed < 100) current.SendFile({}, Region(fixture, 1));
    }
  } file_handler{fixture, completed};
  registry.AddConnection(
      Socket(pair[0]),
      std::bind_front(&FileBudgetHandler::HandleMessage, &file_handler));

  using HandleMessageTaskHealthyCallsState = decltype((healthy_calls));
  struct HandleMessageTask {
    HandleMessageTaskHealthyCallsState healthy_calls;
    decltype(auto) HandleMessage(TcpConnection &connection,
                                 std::span<const std::byte> bytes,
                                 bool) const {
      ++healthy_calls;
      connection.Send(bytes);
      connection.Consume(bytes.size());
    }
  };
  registry.AddConnection(Socket(healthy[0]),
                         std::bind(&HandleMessageTask::HandleMessage,
                                   HandleMessageTask{healthy_calls},
                                   std::placeholders::_1,
                                   std::placeholders::_2,
                                   std::placeholders::_3));
  Require(::send(peer.fd(), "x", 1, MSG_NOSIGNAL) == 1 &&
              ::send(other.fd(), "y", 1, MSG_NOSIGNAL) == 1,
          "fairness ready sockets");
  struct ForceDrainTarget {
    ConnectionRegistry &registry;
    void HandleControl(EventLoop::Control, EventLoop::Deadline) {
      registry.BeginDrain(true);
    }
  } control_target{registry};
  loop.set_HandleControl_callback(
      std::bind_front(&ForceDrainTarget::HandleControl, &control_target));
  bool task = false;

  using VerifyFileBudgetTaskCompletedState = decltype((completed));
  using VerifyFileBudgetTaskHealthyCallsState = decltype((healthy_calls));
  using VerifyFileBudgetTaskState = decltype((task));
  using VerifyFileBudgetTaskLoopState = decltype((loop));
  struct VerifyFileBudgetTask {
    VerifyFileBudgetTaskCompletedState completed;
    VerifyFileBudgetTaskHealthyCallsState healthy_calls;
    VerifyFileBudgetTaskState task;
    VerifyFileBudgetTaskLoopState loop;
    decltype(auto) VerifyFileBudget() const {
      Require(completed == 1 && healthy_calls == 1,
              "outer write loop does not bypass file budget");
      task = true;
      loop.RequestForce();
    }
  };
  Require(loop.QueueInLoop(std::bind(
              &VerifyFileBudgetTask::VerifyFileBudget,
              VerifyFileBudgetTask{completed, healthy_calls, task, loop})),
          "fairness task accepted");
  loop.PollOnce(0);
  Require(task && completed == 1 && ShutdownAccess::entries(registry).empty(),
          "owner task and force run before reentrant file suffix");
  std::cout << "M1 fairness completed_before_control=" << completed
            << " healthy=" << healthy_calls << " task=" << task << '\n';
}

void GuardedSigpipe(const Fixture &fixture, bool disable_peer_close) {
  const pid_t child = ::fork();
  Require(child >= 0, "protected SIGPIPE fork");
  if (child == 0) {
    try {
      struct sigaction action {};

      action.sa_handler = SIG_DFL;
      ::sigemptyset(&action.sa_mask);
      Require(::sigaction(SIGPIPE, &action, nullptr) == 0,
              "default SIGPIPE disposition");
      sigset_t pipe, original, actual, pending;
      ::sigemptyset(&pipe);
      ::sigaddset(&pipe, SIGPIPE);
      ::pthread_sigmask(SIG_UNBLOCK, &pipe, &original);
      for (int mode = 0; mode < 3; ++mode) {
        if (mode) ::pthread_sigmask(SIG_BLOCK, &pipe, nullptr);
        if (mode == 2)
          Require(::pthread_kill(::pthread_self(), SIGPIPE) == 0,
                  "preexisting pending SIGPIPE");
        int pair[2];
        Require(
            ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0,
            "pipe pair");
        Socket disconnected(pair[1]);
        if (!disable_peer_close) disconnected.Reset();
        sigset_t saved_mask;
        ::pthread_sigmask(SIG_SETMASK, nullptr, &saved_mask);
        ConnectionIo io{Socket(pair[0])};
        io.QueueFile({}, Region(fixture, 1));
        const auto result = io.WriteAvailable();
        Require(result.error_number == EPIPE,
                "real sendfile EPIPE returned safely");
        ::pthread_sigmask(SIG_SETMASK, nullptr, &actual);
        ::sigpending(&pending);
        for (int signal = 1; signal < NSIG; ++signal)
          Require(::sigismember(&actual, signal) ==
                      ::sigismember(&saved_mask, signal),
                  "guard full mask restored");
        Require(::sigismember(&actual, SIGPIPE) == (mode ? 1 : 0) &&
                    ::sigismember(&pending, SIGPIPE) == (mode ? 1 : 0),
                "original blocked and pending SIGPIPE preserved");
        if (mode) {
          const timespec zero{};
          Require(::sigtimedwait(&pipe, nullptr, &zero) == SIGPIPE,
                  "consume test-owned pending");
          ::pthread_sigmask(SIG_UNBLOCK, &pipe, nullptr);
        }
      }
      {
        auto listener = Socket::CreateTcp();
        listener.BindAny(0);
        listener.Listen(4);
        Socket reset_peer(ConnectClient(listener.local_port()));
        Socket accepted;
        WaitUntil([&] {
          accepted = listener.AcceptNonBlocking();
          return accepted.valid();
        });
        const linger reset{1, 0};
        Require(::setsockopt(reset_peer.fd(),
                             SOL_SOCKET,
                             SO_LINGER,
                             &reset,
                             sizeof(reset)) == 0,
                "TCP reset peer configuration");
        reset_peer.Reset();
        pollfd event{accepted.fd(), POLLERR, 0};
        const int polled = ::poll(&event, 1, 1000);
        std::cerr << "TCP reset diagnostic fd=" << event.fd
                  << " poll=" << polled << " revents=" << event.revents
                  << " errno=" << errno << std::endl;
        Require(polled == 1 && (event.revents & POLLERR), "TCP reset observed");
        ConnectionIo reset_io(std::move(accepted));
        reset_io.QueueFile({}, Region(fixture, 1));
        const auto reset_result = reset_io.WriteAvailable();
        Require(reset_result.error_number == ECONNRESET ||
                    reset_result.error_number == EPIPE,
                "real TCP reset is connection-local");
        Require(reset_io.WriteAvailable().error_number == EPIPE,
                "TCP reset followed by safe EPIPE");
      }
      int pair[2];
      Require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0,
              "healthy pipe pair");
      Socket peer(pair[1]);
      ConnectionIo healthy{Socket(pair[0])};
      healthy.QueueFile({}, Region(fixture, 1));
      Require(healthy.WriteAvailable().bytes_written == 1,
              "next connection survives SIGPIPE");
      ::sigaction(SIGPIPE, nullptr, &action);
      Require(action.sa_handler == SIG_DFL,
              "transport did not globally ignore SIGPIPE");
      ::pthread_sigmask(SIG_SETMASK, &original, nullptr);
      ::_exit(0);
    } catch (const std::exception &error) {
      std::cerr << "SIGPIPE child FAIL: " << error.what() << std::endl;
      ::_exit(94);
    }
  }
  int status;
  Require(::waitpid(child, &status, 0) == child, "protected SIGPIPE reap");
  std::cout << "SIGPIPE child exit="
            << (WIFEXITED(status) ? WEXITSTATUS(status) : -1)
            << " signal=" << (WIFSIGNALED(status) ? WTERMSIG(status) : 0)
            << std::endl;
  Require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
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

  static void Write(TcpConnection &connection) {
    connection.HandleConnectionEvent(EPOLLOUT);
  }
};
}  // namespace hp::net

namespace {
std::size_t FileCount() {
  std::lock_guard lock(file_evidence_mutex);
  return file_evidence.size();
}

std::size_t OwnerCapacity(ServerHarness &harness,
                          std::size_t workers,
                          std::size_t owner) {
  std::promise<std::size_t> answer;
  auto result = answer.get_future();

  using CollectBufferCapacityTaskHarnessState = decltype((harness));
  using CollectBufferCapacityTaskOwnerState = decltype((owner));
  using CollectBufferCapacityTaskAnswerState = decltype((answer));
  struct CollectBufferCapacityTask {
    CollectBufferCapacityTaskHarnessState harness;
    CollectBufferCapacityTaskOwnerState owner;
    CollectBufferCapacityTaskAnswerState answer;
    decltype(auto) CollectBufferCapacity() const {
      auto &entries = ShutdownAccess::entries(
          SendfileTestAccess::registry(*harness.server, owner));
      Require(entries.size() == 1,
              "one production connection per measured owner");
      answer.set_value(SendfileTestAccess::capacity(*entries.begin()->second));
    }
  };
  auto observe = std::bind(&CollectBufferCapacityTask::CollectBufferCapacity,
                           CollectBufferCapacityTask{harness, owner, answer});
  if (workers)
    Require(TcpServerTestAccess::Post(*harness.server, owner, observe),
            "capacity owner observer");
  else
    Require(TcpServerTestAccess::MainPost(*harness.server, observe),
            "capacity main observer");
  Require(result.wait_for(3s) == std::future_status::ready,
          "capacity observer ready");
  return result.get();
}

std::string MaterializePrepared(hp::http::ResponseResult prepared) {
  if (!prepared.file)
    return {reinterpret_cast<const char *>(prepared.bytes.data()),
            prepared.bytes.size()};
  int pair[2];
  Require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0,
          "prepare comparison pair");
  Socket peer(pair[1]);
  ConnectionIo io{Socket(pair[0])};
  io.QueueFile(prepared.bytes, std::move(*prepared.file));
  std::string wire;
  const auto deadline = Clock::now() + 3s;
  while (io.has_pending_output()) {
    const auto result = io.WriteAvailable();
    Require(result.error_number == 0, "prepared file transfer");
    Collect(peer.fd(), wire);
    Require(Clock::now() < deadline, "prepared file transfer deadline");
  }
  Collect(peer.fd(), wire);
  return wire;
}

void PrepareCompatibility(const Fixture &fixture,
                          const hp::http::StaticFileService &service) {
  Fixture::WriteText(fixture.root_ / "empty.bin", "");
  Fixture::WriteText(fixture.root_ / "one.bin", "X");
  Fixture::WriteBytes(fixture.root_ / "kilo.bin",
                      std::vector<std::byte>(fixture.large_.begin(),
                                             fixture.large_.begin() + 1024));
  Fixture::WriteBytes(
      fixture.root_ / "mega.bin",
      std::vector<std::byte>(fixture.large_.begin(),
                             fixture.large_.begin() + 1024 * 1024));
  for (auto path : {"/empty.bin",
                    "/one.bin",
                    "/large.bin",
                    "/oversized.bin",
                    "/escape.txt",
                    "/../sibling-secret.txt",
                    "/assets",
                    "/missing",
                    "/assets/binary.png"}) {
    const auto before = FileCount();
    auto prepared =
        service.PrepareResponse({"GET", path},
                                hp::http::ConnectionPolicy::kKeepAlive);
    const auto after_prepare = FileCount();
    if (prepared.file) {
      Require(prepared.file->offset() == 0 && prepared.bytes.size() < 256,
              "prepared metadata and small header");
      Require(Evidence(after_prepare - 1).reads == 0 &&
                  Evidence(after_prepare - 1).preads == 0,
              "prepare never reads body");
    }
    const auto actual = MaterializePrepared(std::move(prepared));
    const auto legacy =
        service.HandleResponse({"GET", path},
                               hp::http::ConnectionPolicy::kKeepAlive);
    Require(
        actual.size() == legacy.bytes.size() &&
            std::memcmp(actual.data(), legacy.bytes.data(), actual.size()) == 0,
        "legacy and prepared HTTP bytes match");
    for (auto id = before; id < FileCount(); ++id)
      Require(Evidence(id).closes == 1,
              "prepare/compatibility close every file identity");
  }
  std::cout << "M2 prepare compatibility paths=9 "
               "zero/one/8MiB/overlimit/security byte_exact=1\n";
}

void ChangingFiles(const Fixture &fixture,
                   const hp::http::StaticFileService &service,
                   bool disable_truncate) {
  const auto path = fixture.root_ / "replace.txt";
  const auto next = fixture.root_ / "replacement.txt";
  Fixture::WriteText(path, "old");
  auto opened = service.PrepareResponse({"GET", "/replace.txt"});
  const auto old_id = LatestFile();
  Fixture::WriteText(next, "new contents");
  std::filesystem::rename(next, path);
  Require(MaterializePrepared(std::move(opened)).ends_with("\r\n\r\nold"),
          "rename keeps opened inode");
  auto replaced = service.PrepareResponse({"GET", "/replace.txt"});
  const auto new_id = LatestFile();
  Require(Evidence(old_id).inode != Evidence(new_id).inode &&
              MaterializePrepared(std::move(replaced))
                  .ends_with("\r\n\r\nnew contents"),
          "next request opens replacement inode");
  Fixture::WriteText(path, "grow");
  auto growing = service.PrepareResponse({"GET", "/replace.txt"});
  Fixture::WriteText(path, "grow plus appended data");
  Require(MaterializePrepared(std::move(growing)).ends_with("\r\n\r\ngrow"),
          "growth cannot exceed original Content-Length");
  Fixture::WriteText(path, "truncate original");
  auto truncated = service.PrepareResponse({"GET", "/replace.txt"});
  const auto truncated_id = LatestFile();
  if (!disable_truncate) Fixture::WriteText(path, "");
  EventLoop loop;
  ConnectionRegistry registry(loop, 0);
  int pair[2];
  Require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0,
          "truncate pair");
  Socket peer(pair[1]);

  using HandleMessageTaskTruncatedState = decltype((truncated));
  struct HandleMessageTask {
    HandleMessageTaskTruncatedState truncated;
    decltype(auto) HandleMessage(TcpConnection &connection,
                                 std::span<const std::byte> input,
                                 bool) const {
      connection.Consume(input.size());
      connection.SendFile(truncated.bytes, std::move(*truncated.file));
    }
  };
  registry.AddConnection(Socket(pair[0]),
                         std::bind(&HandleMessageTask::HandleMessage,
                                   HandleMessageTask{truncated},
                                   std::placeholders::_1,
                                   std::placeholders::_2,
                                   std::placeholders::_3));
  Require(::send(peer.fd(), "x", 1, MSG_NOSIGNAL) == 1, "truncate trigger");
  loop.PollOnce(0);
  std::string wire;
  Collect(peer.fd(), wire);
  Require(ShutdownAccess::entries(registry).empty() &&
              Evidence(truncated_id).closes == 1 &&
              wire.starts_with("HTTP/1.1 200") && wire.ends_with("\r\n\r\n") &&
              wire.find("500") == std::string::npos,
          "truncate closes without extra error response");
  std::cout << "M2 file identity rename=old/new growth=initial_length "
               "truncate=header_only_EOF\n";
}

void ProductionFiles(const Fixture &fixture,
                     const hp::http::StaticFileService &service) {
  ScopedAcceptedSendBuffer send_buffer_scope;
  for (std::size_t workers : {0U, 1U, 2U}) {
    const auto begin = FileCount();
    ServerHarness harness(service, workers);
    std::size_t request_index = 0;
    for (const auto &[path, length] :
         std::vector<std::pair<std::string, std::size_t>>{
             {"/kilo.bin", 1024},
             {"/mega.bin", 1024 * 1024},
             {"/large.bin", fixture.large_.size()}}) {
      Stream client(harness.port);
      const auto opening = FileCount();
      client.Send(Query(path));
      WaitUntil([&] {
        if (FileCount() <= opening) return false;
        const auto record = Evidence(opening);
        return length == fixture.large_.size() ? record.eagain > 0
                                               : record.positive > 0;
      });
      const auto owner = workers ? request_index % workers : 0;
      const auto capacity = OwnerCapacity(harness, workers, owner);
      const auto received = client.ReadResponse();
      Require(received.status == 200 && received.body.size() == length &&
                  std::memcmp(received.body.data(),
                              fixture.large_.data(),
                              length) == 0,
              "production size ladder byte exact");
      Require(capacity < 256 && received.header.size() < 256,
              "production memory independent of body size");
      client.Send(Query("/missing", true));
      Response(client.ReadResponse(), 404, "404 Not Found\n");
      client.ExpectEof();
      const auto record = Evidence(opening);
      Require(record.reads == 0 && record.preads == 0 &&
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
    pipeline.Send(Query("/note.txt") + Query("/one.bin"));
    Require(::shutdown(pipeline.fd, SHUT_WR) == 0, "pipeline peer FIN");
    Response(pipeline.ReadResponse(), 200, "hello from S3\n");
    Response(pipeline.ReadResponse(), 200, "X");
    pipeline.ExpectEof();
    harness.Stop();
    for (auto id = begin; id < FileCount(); ++id)
      Require(Evidence(id).closes == 1 && Evidence(id).reads == 0 &&
                  Evidence(id).preads == 0,
              "production each opened region closes exactly once");
  }
  send_buffer_scope.VerifyAndRestore();
}

void FileProgress(const Fixture &fixture,
                  const hp::http::StaticFileService &service) {
  EventLoop loop;
  ConnectionRegistry registry(loop, hp::http::kMaxRequestBytes, {150ms, 100ms});
  int pair[2];
  Require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0,
          "progress pair");
  Socket peer(pair[1]);
  int small = 4096;
  ::setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
  int providers = 0;
  hp::app::HttpCallbackStats stats;
  using ResponseScenario1State0 = decltype((providers));
  using ResponseScenario1State1 = decltype((service));
  struct ResponseScenario1 {
    ResponseScenario1State0 providers;
    ResponseScenario1State1 service;
    hp::http::ResponseResult PrepareResponse(
        const hp::http::HttpRequest &request,
        hp::http::ConnectionPolicy policy) {
      ++providers;
      return service.PrepareResponse(request, policy);
    }
  };
  registry.AddConnection(
      Socket(pair[0]),
      hp::app::MakeHttpCallback(
          std::bind_front(&ResponseScenario1::PrepareResponse,
                          ResponseScenario1{providers, service}),
          &stats));
  auto &connection = *ShutdownAccess::entries(registry).at(pair[0]);
  const auto request = Query("/large.bin") + Query("/note.txt");
  Require(::send(peer.fd(), request.data(), request.size(), MSG_NOSIGNAL) ==
              static_cast<ssize_t>(request.size()),
          "file pipeline trigger");
  loop.PollOnce(0);
  const auto id = LatestFile();
  auto previous = SendfileTestAccess::progress(connection);
  const auto queued = connection.pending_bytes();
  SendfileTestAccess::Write(connection);
  Require(SendfileTestAccess::progress(connection) == previous &&
              connection.pending_bytes() == queued &&
              !SendfileTestAccess::waiting(connection) && providers == 1 &&
              stats.responses == 1,
          "file EAGAIN neither renews idle nor completes pipeline");
  std::string received;
  const auto began = Clock::now();
  for (int cycle = 0; cycle < 10; ++cycle) {
    std::this_thread::sleep_for(20ms);
    Collect(peer.fd(), received);
    SendfileTestAccess::Write(connection);
    Require(SendfileTestAccess::progress(connection) > previous &&
                connection.pending_bytes() > 0,
            "actual sendfile progress refreshes idle");
    previous = SendfileTestAccess::progress(connection);
  }
  Require(Clock::now() - began >= 200ms && providers == 1,
          "progress keeps transfer beyond idle duration");
  while (!ShutdownAccess::entries(registry).empty()) loop.PollOnce(200);
  Require(loop.timer_count() == 0 && Evidence(id).closes == 1 && providers == 1,
          "stalled file idle closes region without pipeline suffix");
  std::cout << "M2 file_progress bytes=" << Evidence(id).bytes
            << " refreshed_cycles=10" << " EAGAIN=" << Evidence(id).eagain
            << " provider=" << providers << " timer=0 idle_closed=1\n";
  (void)fixture;
}

void UnfinishedLifecycle(const hp::http::StaticFileService &service) {
  const auto baseline = SettleThreads({}, {}, "file-baseline", -1);
  const auto fds = Resources("/proc/self/fd");
  const auto files = FileCount();
  const auto sockets = accepted_sockets.load();
  for (int cycle = 0; cycle < 100; ++cycle) {
    ServerHarness harness(service, 2);
    const auto owned = ReadyThreads(harness);
    Stream first(harness.port), second(harness.port);
    first.Send(Query("/large.bin"));
    second.Send(Query("/large.bin"));
    WaitUntil([&] {
      return Pending(harness, 2, 0) > 0 && Pending(harness, 2, 1) > 0;
    });
    if (cycle == 99) {
      struct ThrowWorkerFileFailureTask {
        decltype(auto) ThrowWorkerFileFailure(EventLoop &) const {
          throw std::runtime_error("file fatal original");
        }
      };
      Require(TcpServerTestAccess::Post(
                  *harness.server,
                  0,
                  std::bind(&ThrowWorkerFileFailureTask::ThrowWorkerFileFailure,
                            ThrowWorkerFileFailureTask{},
                            std::placeholders::_1)),
              "file fatal accepted");
      harness.ExpectFailure("file fatal original");
    } else {
      harness.server->RequestGracefulShutdown(Clock::now());
      JoinGraceful(harness);
    }
    SettleThreads(owned, baseline, "file-joined", cycle);
  }
  Require(FileCount() - files == 200 && accepted_sockets - sockets == 200,
          "100 rounds hold two unfinished file responses");
  for (auto id = files; id < FileCount(); ++id)
    Require(Evidence(id).closes == 1 &&
                Evidence(id).bytes < hp::http::kMaxFileBytes,
            "unfinished file identity closed exactly once");
  Require(Resources("/proc/self/fd") == fds && TaskIds() == baseline,
          "unfinished file lifecycle fd and exact TID baseline");
  std::cout << "M2 lifecycle cycles=100 files=200 sockets=200 fd=" << fds << '/'
            << Resources("/proc/self/fd") << " fatal_original_after_join=1\n";
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
    RawObservation(fixture, disabled_error);
    DefaultSigpipeFixture(fixture);
    TransportBounds(fixture, option == "--no-allocation");
    CallBudgetAndMaskFailure(fixture,
                             option == "--no-budget-eintr",
                             option == "--no-mask-error");
    FileRegistrationFailure(fixture, option == "--no-file-registration");
    TransportShortWrites(fixture);
    TransportErrors(fixture, option == "--no-early-eof");
    TransportFairness(fixture);
    GuardedSigpipe(fixture, option == "--no-peer-close");
    FixtureOwner(fixture);
    hp::http::StaticFileService service(fixture.root_.string());
    watched_root = hp::http::StaticFileServiceTestAccess::root_fd(service);
    hp::app::set_RecordSessionEvent_callback(RecordSessionEvent);
    PrepareCompatibility(fixture, service);
    ChangingFiles(fixture, service, option == "--no-truncate");
    ProductionFiles(fixture, service);
    FileProgress(fixture, service);
    LibraryDrain(service, fixture);
    DeadlineDrain(service, fixture);
    UnfinishedLifecycle(service);
    hp::app::set_RecordSessionEvent_callback(nullptr);
    Require(root_errors == 0 && live_sessions == 0,
            "file session/service owner lifetime");
    {
      std::lock_guard lock(file_evidence_mutex);
      Require(active_files.empty(), "M0 all observed file identities closed");
      for (const auto &item : file_evidence)
        Require(item.closes == 1, "M0 each file identity closed once");
    }
    std::cout << "sendfile_tests M2: PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
