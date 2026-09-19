#include <functional>
#include <type_traits>
// Reuse only the established fixture/client-process utilities and old
// assertions.
#define main legacy_http_integration_entry
#include "http_server_integration_tests.cpp"
#undef main
#include <atomic>
#include <barrier>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <source_location>
#include <thread>

#include "http_connection_handler.h"
#include "net/tcp_server.h"

using namespace hp::net;
using namespace std::chrono_literals;

namespace hp::http {
struct StaticFileServiceTestAccess {
  static int root_fd(const StaticFileService& service) {
    return service.root_fd_;
  }
};
}  // namespace hp::http

namespace hp::net {
struct EventLoopThreadPoolTestAccess {
  static std::size_t outstanding(EventLoopThreadPool& pool, std::size_t index) {
    std::lock_guard lock(pool.mutex_);
    return pool.outstanding_.at(index);
  }
};

struct TcpServerTestAccess {
  static bool Post(TcpServer& server,
                   std::size_t index,
                   EventLoopThread::LoopTask task) {
    return server.pool_.Post(index, std::move(task));
  }

  static std::size_t outstanding(TcpServer& server, std::size_t index) {
    return EventLoopThreadPoolTestAccess::outstanding(server.pool_, index);
  }

  static bool MainPost(TcpServer& server, EventLoop::LoopTask task) {
    return server.loop_.QueueInLoop(std::move(task));
  }
};
}  // namespace hp::net

namespace {
void MultiReactorCompleteLoopProbe(EventLoop&) {}

thread_local int timer_allocation_failure = -1;
thread_local int fail_after_registration = -1;
thread_local bool reject_any_registration = false;
thread_local unsigned int registration_injections = 0;
std::mutex socket_probe_mutex;
std::set<int> accepted_fds;
std::atomic<int> accepted_sockets{}, closed_sockets{}, invalid_closes{},
    reject_registration{}, reject_allocation{};
// Disabled outside the two real-backpressure fixture scopes.
std::atomic<int> accepted_send_buffer_bytes{0};
std::atomic<int> accepted_send_buffer_error{0};
std::atomic<unsigned> configured_send_buffers{0};
constexpr int kAcceptedSendBufferBytes = 16384;
constexpr int kEffectiveSendBufferBytes = 32768;

void ConfigureAcceptedSendBuffer(int accepted) {
  const int requested = accepted_send_buffer_bytes.load();
  if (requested == 0) return;
  int effective = 0;
  socklen_t length = sizeof(effective);
  int error = 0;
  if (::setsockopt(accepted,
                   SOL_SOCKET,
                   SO_SNDBUF,
                   &requested,
                   sizeof(requested)) < 0) {
    error = errno;
  } else if (::getsockopt(accepted,
                          SOL_SOCKET,
                          SO_SNDBUF,
                          &effective,
                          &length) < 0) {
    error = errno;
  } else if (length != sizeof(effective) ||
             effective != kEffectiveSendBufferBytes) {
    error = EPROTO;
  }
  if (error != 0) {
    accepted_send_buffer_error.store(error);
    // The fd was accepted and counted, but has not been handed to Acceptor.
    ::close(accepted);
    std::cerr << "accepted SO_SNDBUF fixture failed errno=" << error
              << " effective=" << effective << '\n';
    throw std::system_error(error,
                            std::generic_category(),
                            "accepted SO_SNDBUF fixture");
  }
  ++configured_send_buffers;
}
}  // namespace

extern "C" {
int __real_accept4(int, sockaddr*, socklen_t*, int);

int __wrap_accept4(int fd, sockaddr* address, socklen_t* size, int flags) {
  const int accepted = __real_accept4(fd, address, size, flags);
  if (accepted >= 0) {
    {
      std::lock_guard lock(socket_probe_mutex);
      accepted_fds.insert(accepted);
      ++accepted_sockets;
    }
    ConfigureAcceptedSendBuffer(accepted);
  }
  return accepted;
}

int __real_close(int);

int __wrap_close(int fd) {
  {
    std::lock_guard lock(socket_probe_mutex);
    if (accepted_fds.erase(fd)) ++closed_sockets;
  }
  const int result = __real_close(fd);
  if (result < 0 && errno == EBADF) ++invalid_closes;
  return result;
}

int __real_epoll_ctl(int, int, int, epoll_event*);

int __wrap_epoll_ctl(int fd,
                     int operation,
                     int observed_fd,
                     epoll_event* event) {
  if (operation == EPOLL_CTL_ADD &&
      std::exchange(reject_any_registration, false)) {
    ++registration_injections;
    errno = EIO;
    return -1;
  }
  if (operation == EPOLL_CTL_ADD && reject_registration.load()) {
    std::lock_guard lock(socket_probe_mutex);
    if (accepted_fds.contains(observed_fd) && reject_registration.exchange(0)) {
      errno = EIO;
      return -1;
    }
  }
  const int result = __real_epoll_ctl(fd, operation, observed_fd, event);
  if (result == 0 && operation == EPOLL_CTL_ADD &&
      fail_after_registration >= 0) {
    timer_allocation_failure = fail_after_registration;
    fail_after_registration = -1;
  }
  return result;
}

void* __real__Znwm(std::size_t);

void* __wrap__Znwm(std::size_t size) {
  if (timer_allocation_failure == 0) {
    timer_allocation_failure = -1;
    throw std::bad_alloc();
  }
  if (timer_allocation_failure > 0) --timer_allocation_failure;
  if (size == sizeof(TcpConnection) && reject_allocation.exchange(0))
    throw std::bad_alloc();
  return __real__Znwm(size);
}
}

namespace {
void Require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

class ScopedAcceptedSendBuffer {
 public:
  ScopedAcceptedSendBuffer()
      : previous_(
            accepted_send_buffer_bytes.exchange(kAcceptedSendBufferBytes)),
        configured_before_(configured_send_buffers.load()) {
    accepted_send_buffer_error.store(0);
  }

  ~ScopedAcceptedSendBuffer() {
    if (active_) accepted_send_buffer_bytes.store(previous_);
  }

  ScopedAcceptedSendBuffer(const ScopedAcceptedSendBuffer&) = delete;
  ScopedAcceptedSendBuffer& operator=(const ScopedAcceptedSendBuffer&) = delete;

  // Call only after all servers/connections created in this scope are
  // destroyed.
  void VerifyAndRestore() {
    const int requested = accepted_send_buffer_bytes.exchange(previous_);
    active_ = false;
    Require(requested == kAcceptedSendBufferBytes &&
                accepted_send_buffer_bytes.load() == previous_,
            "accepted send buffer scope restores configuration");
    Require(accepted_send_buffer_error.load() == 0 &&
                configured_send_buffers.load() > configured_before_,
            "accepted SO_SNDBUF fixture configured and verified");
    std::cout << "accepted SO_SNDBUF requested=" << kAcceptedSendBufferBytes
              << " verified=" << kEffectiveSendBufferBytes << " sockets="
              << configured_send_buffers.load() - configured_before_
              << " restored=" << previous_ << '\n';
  }

 private:
  int previous_;
  unsigned configured_before_;
  bool active_{true};
};

template <class F>
void WaitUntil(F predicate,
               std::source_location caller = std::source_location::current()) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!predicate()) {
    try {
      Require(std::chrono::steady_clock::now() < deadline, "deadline");
    } catch (...) {
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - (deadline - 3s));
      std::cerr << "await deadline file=" << caller.file_name()
                << " line=" << caller.line()
                << " elapsed_us=" << elapsed.count() << std::endl;
      throw;
    }
    std::this_thread::yield();
  }
}

std::size_t Resources(const char* path) {
  return static_cast<std::size_t>(
      std::distance(std::filesystem::directory_iterator(path), {}));
}

struct MessageObservation {
  int fd{-1};
  TcpConnection::Identity identity{};
  sockaddr_in peer{};
  int peer_error{};
  std::thread::id owner;
  TcpConnection::State state{TcpConnection::State::kUnregistered};
  bool eof{}, completed{};
  std::size_t calls{};
  std::string input;
};

struct Probe {
  std::mutex mutex;
  std::function<void(TcpConnection&, std::span<const std::byte>, bool)>
      before_message;
  std::thread::id main;
  std::vector<std::thread::id> factories, owners;
  std::vector<MessageObservation> messages;
  std::map<const void*, std::thread::id> sessions;
  int created{}, destroyed{}, owner_errors{}, callback_releases{};
};

std::atomic<Probe*> observed{nullptr};
std::atomic<int> watched_root{-1}, root_checks{}, root_errors{},
    live_sessions{};

void RecordSessionEvent(bool created, const void* session) noexcept {
  ++root_checks;
  if (::fcntl(watched_root.load(), F_GETFD) < 0) ++root_errors;
  if (created)
    ++live_sessions;
  else
    --live_sessions;
  if (auto* probe = observed.load()) {
    std::lock_guard lock(probe->mutex);
    if (created) {
      if (std::find(probe->owners.begin(),
                    probe->owners.end(),
                    std::this_thread::get_id()) == probe->owners.end())
        ++probe->owner_errors;
      probe->sessions.emplace(session, std::this_thread::get_id());
      ++probe->created;
    } else {
      if (probe->sessions.at(session) != std::this_thread::get_id())
        ++probe->owner_errors;
      probe->sessions.erase(session);
      ++probe->destroyed;
    }
  }
}

struct ObservedHttpMessage {
  TcpConnection::MessageCallback message_callback;
  Probe* probe;
  std::size_t id;

  void set_HandleMessage_callback(TcpConnection::MessageCallback callback) {
    message_callback = std::move(callback);
  }
  void HandleMessage(TcpConnection& connection,
                     std::span<const std::byte> bytes,
                     bool eof) {
    {
      std::lock_guard lock(probe->mutex);
      auto& owner = probe->owners[id];
      if (owner != std::thread::id{} && owner != std::this_thread::get_id())
        ++probe->owner_errors;
      owner = std::this_thread::get_id();
      auto& message = probe->messages[id];
      message.fd = connection.fd();
      message.identity = connection.identity();
      socklen_t size = sizeof(message.peer);
      message.peer_error =
          ::getpeername(message.fd,
                        reinterpret_cast<sockaddr*>(&message.peer),
                        &size) == 0
              ? 0
              : errno;
      message.owner = owner;
      message.state = connection.state();
      message.eof = eof;
      message.completed = false;
      ++message.calls;
      if (!bytes.empty())
        message.input.append(
            reinterpret_cast<const char*>(bytes.data()),
            std::min(bytes.size(), std::size_t{128} - message.input.size()));
    }
    if (probe->before_message) probe->before_message(connection, bytes, eof);
    message_callback(connection, bytes, eof);
    {
      std::lock_guard lock(probe->mutex);
      probe->messages[id].completed = true;
      probe->messages[id].state = connection.state();
    }
  }
};

struct ObservedHttpFactory {
  hp::app::HttpMessageFactory production;
  Probe* probe;
  TcpConnection::MessageCallback CreateMessageCallback() {
    auto callback = production.CreateMessageCallback();
    if (!probe) return callback;
    std::size_t id;
    {
      std::lock_guard lock(probe->mutex);
      probe->factories.push_back(std::this_thread::get_id());
      id = probe->owners.size();
      probe->owners.push_back({});
      probe->messages.emplace_back();
    }
    ObservedHttpMessage observer{{}, probe, id};
    observer.set_HandleMessage_callback(std::move(callback));
    return std::bind_front(&ObservedHttpMessage::HandleMessage,
                           std::move(observer));
  }
};

struct ServerHarness {
  std::thread control;
  TcpServer* server{};
  std::uint16_t port{};
  std::exception_ptr error;
  std::mutex lifetime_mutex;
  std::condition_variable lifetime_ready;
  bool release_owner{false};
  std::atomic<bool> run_ended{false}, run_failed{false};

  void RunServerOwner(const hp::http::StaticFileService& service,
                      std::size_t workers,
                      Probe* probe,
                      ConnectionTimeouts timeouts,
                      std::promise<void>& ready) {
    bool published = false;
    try {
      TcpServer instance(0, hp::http::kMaxRequestBytes, workers, timeouts);
      instance.set_CreateMessageCallback_callback(
          std::bind_front(&ObservedHttpFactory::CreateMessageCallback,
                          ObservedHttpFactory{{service}, probe}));
      if (probe) probe->main = std::this_thread::get_id();
      server = &instance;
      port = instance.bound_port();
      published = true;
      ready.set_value();
      try {
        instance.Run();
      } catch (...) {
        error = std::current_exception();
      }
      run_failed = error != nullptr;
      run_ended = true;
      // Keep the borrowed server alive until the caller ends all API calls.
      // Destruction still occurs on this owner, after explicit join release.
      std::unique_lock lifetime_lock(lifetime_mutex);
      lifetime_ready.wait(lifetime_lock, [this] { return release_owner; });
    } catch (...) {
      error = std::current_exception();
      if (!published) ready.set_exception(error);
    }
  }

  ServerHarness(const hp::http::StaticFileService& service,
                std::size_t workers,
                Probe* probe = nullptr,
                ConnectionTimeouts timeouts = {}) {
    std::promise<void> ready;
    auto result = ready.get_future();
    control = std::thread(&ServerHarness::RunServerOwner,
                          this,
                          std::cref(service),
                          workers,
                          probe,
                          timeouts,
                          std::ref(ready));
    try {
      Require(result.wait_for(3s) == std::future_status::ready,
              "server ready deadline");
      result.get();
    } catch (...) {
      Join();
      throw;
    }
  }

  ~ServerHarness() {
    if (control.joinable()) {
      server->RequestStop();
      Join();
    }
  }

  void Join() {
    {
      std::lock_guard lock(lifetime_mutex);
      release_owner = true;
    }
    lifetime_ready.notify_all();
    control.join();
  }

  void Stop() {
    server->RequestStop();
    Join();
    if (error) std::rethrow_exception(error);
  }

  void ExpectFailure(const char* message) {
    Join();
    Require(error != nullptr, "server must report failure");
    try {
      std::rethrow_exception(error);
    } catch (const std::runtime_error& caught) {
      Require(std::string(caught.what()) == message,
              "first fatal exception preserved");
    }
  }
};

struct WireResponse {
  int status;
  std::string header, body;
};

struct Stream {
  int fd;
  std::string pending;

  explicit Stream(std::uint16_t port) : fd(ConnectClient(port)) {}

  ~Stream() { ::close(fd); }

  void Send(std::string_view data) { SendAll(fd, data); }

  void ReadMoreResponseBytes() {
    char bytes[8192];
    auto n = ::recv(fd, bytes, sizeof(bytes), 0);
    Require(n > 0, "response recv");
    pending.append(bytes, static_cast<std::size_t>(n));
  }

  WireResponse ReadResponse() {
    while (pending.find("\r\n\r\n") == std::string::npos)
      ReadMoreResponseBytes();
    const auto boundary = pending.find("\r\n\r\n") + 4;
    auto header = pending.substr(0, boundary);
    const auto at = header.find("Content-Length: ");
    Require(at != std::string::npos, "content length");
    const auto length = std::stoull(header.substr(at + 16));
    while (pending.size() < boundary + length) ReadMoreResponseBytes();
    auto body = pending.substr(boundary, length);
    pending.erase(0, boundary + length);
    return {std::stoi(header.substr(9, 3)), std::move(header), std::move(body)};
  }

  void ExpectEof() {
    char byte;
    Require(pending.empty() && ::recv(fd, &byte, 1, 0) == 0, "terminal EOF");
  }
};

std::string Query(std::string_view path, bool close = false) {
  return "GET " + std::string(path) + " HTTP/1.1\r\nHost: localhost\r\n" +
         (close ? "Connection: close\r\n" : "") + "\r\n";
}

void Response(const WireResponse& value, int status, std::string_view body) {
  Require(value.status == status && value.body == body,
          "exact response status/body");
}

void Owners(const hp::http::StaticFileService& service, std::size_t workers) {
  Probe probe;
  observed = &probe;
  ServerHarness harness(service, workers, &probe);
  for (int i = 0; i < 6; ++i) {
    Stream client(harness.port);
    client.Send(Query("/note.txt", true));
    Response(client.ReadResponse(), 200, "hello from S3\n");
    client.ExpectEof();
  }
  harness.Stop();
  observed = nullptr;
  Require(
      probe.factories.size() == 6 && probe.created == 6 && probe.destroyed == 6,
      "session accounting");
  for (int i = 0; i < 6; ++i) {
    Require(probe.factories[i] == probe.main, "factory main owner");
    Require(probe.owners[i] == probe.owners[i % (workers ? workers : 1)],
            "fixed round robin");
    Require((probe.owners[i] == probe.main) == (workers == 0),
            "worker mode owner");
  }
  if (workers == 2)
    Require(probe.owners[0] != probe.owners[1], "two workers have real IO");
  Require(probe.owner_errors == 0 && probe.sessions.empty(),
          "session lifetime owner");
  std::cout << "owners: workers=" << workers
            << " accepted=" << probe.factories.size()
            << " session_created=" << probe.created
            << " destroyed=" << probe.destroyed << " route=";
  for (auto owner : probe.owners)
    std::cout << (owner == probe.owners[0] ? '0' : '1');
  std::cout << '\n';
}

void Protocols(const hp::http::StaticFileService& service,
               ConnectionTimeouts timeouts = {}) {
  ServerHarness harness(service, 2, nullptr, timeouts);
  std::barrier begin(3);
  std::exception_ptr errors[2];
  std::thread clients[2];
  for (int i = 0; i < 2; ++i) {
    using ExchangeConcurrentRequestsTaskIState =
        std::remove_cvref_t<decltype(i)>;
    using ExchangeConcurrentRequestsTaskHarnessState = decltype((harness));
    using ExchangeConcurrentRequestsTaskBeginState = decltype((begin));
    using ExchangeConcurrentRequestsTaskErrorsState = decltype((errors));
    struct ExchangeConcurrentRequestsTask {
      ExchangeConcurrentRequestsTaskIState i;
      ExchangeConcurrentRequestsTaskHarnessState harness;
      ExchangeConcurrentRequestsTaskBeginState begin;
      ExchangeConcurrentRequestsTaskErrorsState errors;
      decltype(auto) ExchangeConcurrentRequests() const {
        try {
          Stream client(harness.port);
          begin.arrive_and_wait();
          auto first = Query(i ? "/" : "/note.txt");
          client.Send(first.substr(0, first.size() - 1));
          client.Send(first.substr(first.size() - 1) + Query("/missing") +
                      Query("/note.txt"));
          Response(client.ReadResponse(),
                   200,
                   i ? "<h1>integration index</h1>\n" : "hello from S3\n");
          Response(client.ReadResponse(), 404, "404 Not Found\n");
          Response(client.ReadResponse(), 200, "hello from S3\n");
          client.Send("GET / HTTP/1.1\r\nBad\r\n\r\n");
          Response(client.ReadResponse(), 400, "400 Bad Request\n");
          client.ExpectEof();
        } catch (...) {
          errors[i] = std::current_exception();
        }
      }
    };
    clients[i] = std::thread(
        std::bind(&ExchangeConcurrentRequestsTask::ExchangeConcurrentRequests,
                  ExchangeConcurrentRequestsTask{i, harness, begin, errors}));
  }
  begin.arrive_and_wait();
  for (auto& client : clients) client.join();
  for (auto error : errors)
    if (error) std::rethrow_exception(error);
  {
    Stream fin(harness.port);
    fin.Send(Query("/note.txt") + Query("/note.txt"));
    ::shutdown(fin.fd, SHUT_WR);
    Response(fin.ReadResponse(), 200, "hello from S3\n");
    Response(fin.ReadResponse(), 200, "hello from S3\n");
    fin.ExpectEof();
  }
  {
    Stream reset(harness.port);
    reset.Send("GET / HTTP/1.1\r\nHost:");
    linger immediate{1, 0};
    ::setsockopt(reset.fd,
                 SOL_SOCKET,
                 SO_LINGER,
                 &immediate,
                 sizeof(immediate));
  }
  for (auto path : {"/escape.txt", "/../sibling-secret.txt"}) {
    Stream client(harness.port);
    client.Send(Query(path, true));
    Response(client.ReadResponse(), 403, "403 Forbidden\n");
    client.ExpectEof();
  }
  Stream survivor(harness.port);
  survivor.Send(Query("/note.txt", true));
  Response(survivor.ReadResponse(), 200, "hello from S3\n");
  survivor.ExpectEof();
  harness.Stop();
  std::cout << "protocols: concurrent_pipeline=2 per_pipeline_responses=4 "
               "fin_responses=2 "
               "rst_survivor=1 security=2\n";
}

void Lifecycle(const hp::http::StaticFileService& service) {
  auto fd = Resources("/proc/self/fd");
  auto threads = Resources("/proc/self/task");
  for (int i = 0; i < 100; ++i) {
    ServerHarness harness(service, 2);
    Stream client(harness.port);
    client.Send(Query("/note.txt"));
    Response(client.ReadResponse(), 200, "hello from S3\n");
    harness.Stop();
    client.ExpectEof();
  }
  WaitUntil([&] { return Resources("/proc/self/task") == threads; });
  Require(Resources("/proc/self/fd") == fd, "100 cycle fd baseline");
  std::cout << "lifecycle: cycles=100 fd=" << fd << '/'
            << Resources("/proc/self/fd") << " threads=" << threads << '/'
            << Resources("/proc/self/task") << '\n';
}

void FailuresAndPending(const hp::http::StaticFileService& service) {
  {
    ServerHarness harness(service, 2);
    std::promise<void> entered, release;
    auto gate = release.get_future().share();

    using WaitForReleaseTaskEnteredState = decltype((entered));
    using WaitForReleaseTaskGateState = decltype((gate));
    struct WaitForReleaseTask {
      WaitForReleaseTaskEnteredState entered;
      WaitForReleaseTaskGateState gate;
      decltype(auto) WaitForRelease(EventLoop&) const {
        entered.set_value();
        Require(gate.wait_for(3s) == std::future_status::ready, "fatal gate");
        throw std::runtime_error("fatal worker original");
      }
    };
    Require(
        TcpServerTestAccess::Post(*harness.server,
                                  0,
                                  std::bind(&WaitForReleaseTask::WaitForRelease,
                                            WaitForReleaseTask{entered, gate},
                                            std::placeholders::_1)),
        "fatal task accepted");
    Require(entered.get_future().wait_for(3s) == std::future_status::ready,
            "fatal worker entered");
    for (int i = 1; i < 1024; ++i)
      Require(TcpServerTestAccess::Post(*harness.server,
                                        0,
                                        &MultiReactorCompleteLoopProbe),
              "fill worker queue");
    Require(!TcpServerTestAccess::Post(*harness.server,
                                       0,
                                       &MultiReactorCompleteLoopProbe),
            "full before failure");
    release.set_value();
    harness.ExpectFailure("fatal worker original");
  }
  {
    ServerHarness harness(service, 2);

    struct ThrowMainFailureTask {
      decltype(auto) ThrowMainFailure() const {
        throw std::runtime_error("fatal main original");
      }
    };
    TcpServerTestAccess::MainPost(
        *harness.server,
        std::bind(&ThrowMainFailureTask::ThrowMainFailure,
                  ThrowMainFailureTask{}));
    harness.ExpectFailure("fatal main original");
  }
  {
    ServerHarness harness(service, 2);
    Stream active(harness.port);
    active.Send(Query("/note.txt"));
    Response(active.ReadResponse(), 200, "hello from S3\n");
    std::promise<void> entered, release;
    auto gate = release.get_future().share();

    using WaitForPendingReleaseTaskEnteredState = decltype((entered));
    using WaitForPendingReleaseTaskGateState = decltype((gate));
    struct WaitForPendingReleaseTask {
      WaitForPendingReleaseTaskEnteredState entered;
      WaitForPendingReleaseTaskGateState gate;
      decltype(auto) WaitForPendingRelease(EventLoop&) const {
        entered.set_value();
        Require(gate.wait_for(3s) == std::future_status::ready, "pending gate");
      }
    };
    TcpServerTestAccess::Post(
        *harness.server,
        1,
        std::bind(&WaitForPendingReleaseTask::WaitForPendingRelease,
                  WaitForPendingReleaseTask{entered, gate},
                  std::placeholders::_1));
    Require(entered.get_future().wait_for(3s) == std::future_status::ready,
            "pending worker blocked");
    Stream pending(harness.port);
    WaitUntil([&] {
      return TcpServerTestAccess::outstanding(*harness.server, 1) == 2;
    });
    harness.server->RequestStop();
    release.set_value();
    harness.Stop();
    active.ExpectEof();
    pending.ExpectEof();
  }
  std::cout << "failure: full_worker_and_main_originals=2 pending_stop=1 "
               "all_joined\n";
}

void AdoptionFailures(const hp::http::StaticFileService& service) {
  auto accepted = accepted_sockets.load();
  auto closed = closed_sockets.load();
  {
    ServerHarness harness(service, 2);
    reject_registration = 1;
    Stream rejected(harness.port);
    rejected.ExpectEof();
    Stream survivor(harness.port);
    survivor.Send(Query("/note.txt", true));
    Response(survivor.ReadResponse(), 200, "hello from S3\n");
    survivor.ExpectEof();
    harness.Stop();
  }
  Require(accepted_sockets - accepted == 2 && closed_sockets - closed == 2,
          "registration failure closes once and peer survives");
  accepted = accepted_sockets.load();
  closed = closed_sockets.load();
  {
    ServerHarness harness(service, 2);
    reject_allocation = 1;
    Stream rejected(harness.port);
    rejected.ExpectEof();
    harness.Join();
    Require(harness.error != nullptr, "allocation failure propagated");
    bool bad_alloc = false;
    try {
      std::rethrow_exception(harness.error);
    } catch (const std::bad_alloc&) {
      bad_alloc = true;
    }
    Require(bad_alloc, "allocation original type");
  }
  Require(accepted_sockets - accepted == 1 && closed_sockets - closed == 1,
          "allocation closes exactly once");
  accepted = accepted_sockets.load();
  closed = closed_sockets.load();
  {
    ServerHarness harness(service, 1);
    std::promise<void> entered, release;
    auto gate = release.get_future().share();

    using WaitForAdmissionReleaseTaskEnteredState = decltype((entered));
    using WaitForAdmissionReleaseTaskGateState = decltype((gate));
    struct WaitForAdmissionReleaseTask {
      WaitForAdmissionReleaseTaskEnteredState entered;
      WaitForAdmissionReleaseTaskGateState gate;
      decltype(auto) WaitForAdmissionRelease(EventLoop&) const {
        entered.set_value();
        Require(gate.wait_for(3s) == std::future_status::ready, "reject gate");
      }
    };
    TcpServerTestAccess::Post(
        *harness.server,
        0,
        std::bind(&WaitForAdmissionReleaseTask::WaitForAdmissionRelease,
                  WaitForAdmissionReleaseTask{entered, gate},
                  std::placeholders::_1));
    Require(entered.get_future().wait_for(3s) == std::future_status::ready,
            "reject entered");
    for (int i = 1; i < 1024; ++i)
      Require(TcpServerTestAccess::Post(*harness.server,
                                        0,
                                        &MultiReactorCompleteLoopProbe),
              "reject fill");
    Stream rejected(harness.port);
    rejected.ExpectEof();
    release.set_value();
    harness.Stop();
  }
  Require(accepted_sockets - accepted == 1 && closed_sockets - closed == 1,
          "handoff rejection closes exactly once");
  std::cout << "adoption: register_failure=1 allocation_failure=1 "
               "capacity_rejection=1 "
               "exact_close=verified\n";
}

void CliModes(const char* executable, const Fixture& fixture) {
  const char* configured = std::getenv("HP_HTTP_TEST_THREADS");
  const auto previous =
      configured ? std::optional<std::string>(configured) : std::nullopt;
  for (int count : {-1, 0, 1, 2}) {
    if (count < 0)
      ::unsetenv("HP_HTTP_TEST_THREADS");
    else
      ::setenv("HP_HTTP_TEST_THREADS", std::to_string(count).c_str(), 1);
    auto server = StartServer(executable, fixture.root_);
#if defined(__SANITIZE_THREAD__)
    constexpr std::size_t kSanitizerThreads = 1;
    constexpr std::size_t kControllerHelper = 1;
#else
    constexpr std::size_t kSanitizerThreads = 0;
    constexpr std::size_t kControllerHelper = 0;
#endif
    Require(Resources("/proc/self/task") == 1 + kControllerHelper,
            "controller thread baseline matches instrumentation");
    const auto expected =
        static_cast<std::size_t>((count < 0 ? 2 : count) + 1) + 1 +
        kSanitizerThreads;
    std::cout << "CLI logger_threads=1 instrumentation_threads="
              << kSanitizerThreads << " expected=" << expected
              << " observed=" << server.thread_count() << '\n';
    Require(server.thread_count() == expected,
            "production CLI OS thread count");
    Stream client(server.port());
    client.Send(Query("/note.txt", true));
    Response(client.ReadResponse(), 200, "hello from S3\n");
    client.ExpectEof();
    std::cout << "CLI: threads_option=" << count
              << " OS_threads=" << server.thread_count() << '\n';
  }
  if (previous)
    ::setenv("HP_HTTP_TEST_THREADS", previous->c_str(), 1);
  else
    ::unsetenv("HP_HTTP_TEST_THREADS");
}

}  // namespace

#ifndef HP_MULTI_REACTOR_ENTRY
#define HP_MULTI_REACTOR_ENTRY main
#endif
int HP_MULTI_REACTOR_ENTRY(int argc, char** argv) {
  try {
    Fixture fixture;
    auto service_owner =
        std::make_unique<hp::http::StaticFileService>(fixture.root_.string());
    auto& service = *service_owner;
    watched_root = hp::http::StaticFileServiceTestAccess::root_fd(service);
    hp::app::set_RecordSessionEvent_callback(RecordSessionEvent);
    Owners(service, 0);
    Owners(service, 1);
    Owners(service, 2);
    Protocols(service);
    FailuresAndPending(service);
    AdoptionFailures(service);
    Lifecycle(service);
    if (argc == 2) CliModes(argv[1], fixture);
    hp::app::set_RecordSessionEvent_callback(nullptr);
    Require(live_sessions == 0 && root_checks > 0 && root_errors == 0,
            "service outlives all sessions");
    service_owner.reset();
    Require(::fcntl(watched_root, F_GETFD) == -1 && errno == EBADF,
            "service root closed after join");
    std::cout << "service_lifetime: session_events=" << root_checks
              << " premature_close=" << root_errors
              << " live_sessions=" << live_sessions
              << " root_closed_after_join=1\n";
    Require(invalid_closes == 0 && accepted_sockets == closed_sockets,
            "accepted socket close accounting");
    std::cout << "socket_ownership: accepted=" << accepted_sockets
              << " closed=" << closed_sockets
              << " invalid_closes=" << invalid_closes << '\n';
    std::cout << "multi_reactor_tests: PASS\n";
    return failures ? 1 : 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
