#include <dlfcn.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <sstream>

#include "ManagementServer.h"
#include "ServerConfig.h"
#include "TestCheck.h"

using namespace std::chrono_literals;
namespace injection {
enum class Mode { kNone, kCreate, kInitialize, kTimeout, kRun };
using ThreadEntry = void* (*)(void*);
using Create = int (*)(pthread_t*, const pthread_attr_t*, ThreadEntry, void*);
Create nextCreate{};
std::atomic<bool> armed{false};
pthread_t caller{};
Mode mode{};
std::atomic<unsigned> createHits{0}, epollHits{0}, targetExits{0};
std::atomic<unsigned long> targetThread{0};
thread_local Mode targetMode{Mode::kNone};
struct Context {
  ThreadEntry entry{};
  void* argument{};
  Mode mode{};
} context;
std::mutex mutex;
std::condition_variable changed;
bool entered{}, released{};
std::atomic<bool> gateExpired{false};

void* runTarget(void* raw) {
  auto& saved = *static_cast<Context*>(raw);
  targetThread = pthread_self();
  targetMode = saved.mode;
  void* result = saved.entry(saved.argument);
  targetMode = Mode::kNone;
  ++targetExits;
  return result;
}

void waitGate() {
  std::unique_lock lock(mutex);
  entered = true;
  changed.notify_all();
  if (!changed.wait_for(lock, 5s, [] { return released; })) gateExpired = true;
}

void arm(Mode requested) {
  std::lock_guard lock(mutex);
  entered = released = false;
  gateExpired = false;
  createHits = epollHits = targetExits = 0;
  targetThread = 0;
  mode = requested;
  caller = pthread_self();
  armed = true;
}

void release() {
  armed = false;
  std::lock_guard lock(mutex);
  released = true;
  changed.notify_all();
}

void awaitEntry() {
  std::unique_lock lock(mutex);
  if (!changed.wait_for(lock, 5s, [] { return entered; }))
    throw std::runtime_error("injection gate missing");
}
}  // namespace injection

extern "C" int pthread_create(pthread_t* thread,
                              const pthread_attr_t* attributes,
                              injection::ThreadEntry entry,
                              void* argument) {
  using namespace injection;
  if (!nextCreate) std::abort();
  bool expected = true;
  if (armed.load() && pthread_equal(pthread_self(), caller) &&
      armed.compare_exchange_strong(expected, false)) {
    ++createHits;
    if (mode == Mode::kCreate) return EAGAIN;
    context = {entry, argument, mode};
    return nextCreate(thread, attributes, runTarget, &context);
  }
  return nextCreate(thread, attributes, entry, argument);
}

extern "C" int __real_epoll_create1(int flags);
extern "C" int __real_epoll_wait(int fd, epoll_event* events, int count, int timeout);
extern "C" int __wrap_epoll_create1(int flags) {
  using namespace injection;
  if (targetMode == Mode::kInitialize) {
    targetMode = Mode::kNone;
    ++epollHits;
    errno = EMFILE;
    return -1;
  }
  if (targetMode == Mode::kTimeout) {
    targetMode = Mode::kNone;
    ++epollHits;
    waitGate();
  }
  return __real_epoll_create1(flags);
}

extern "C" int __wrap_epoll_wait(int fd, epoll_event* events, int count, int timeout) {
  using namespace injection;
  if (targetMode == Mode::kRun) {
    targetMode = Mode::kNone;
    ++epollHits;
    waitGate();
    errno = EIO;
    return -1;
  }
  return __real_epoll_wait(fd, events, count, timeout);
}

namespace growth {
std::atomic<bool> armed{false};
pthread_t caller{};
dev_t device{};
ino_t inode{};
int writer{-1}, observedReader{-1};
unsigned hits{};
off_t before{}, after{};
bool writeFailed{};
}  // namespace growth

extern "C" int __real_fstat(int fd, struct stat* value);
extern "C" int __wrap_fstat(int fd, struct stat* value) {
  const int result = __real_fstat(fd, value);
  if (result != 0 || !growth::armed.load() || !pthread_equal(pthread_self(), growth::caller) ||
      fd == growth::writer || !S_ISREG(value->st_mode) || value->st_dev != growth::device ||
      value->st_ino != growth::inode)
    return result;
  bool expected = true;
  if (!growth::armed.compare_exchange_strong(expected, false)) return result;
  ++growth::hits;
  growth::observedReader = fd;
  growth::before = value->st_size;
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  ssize_t written;
  do {
    written = ::write(growth::writer, " ", 1);
  } while (written < 0 && errno == EINTR && std::chrono::steady_clock::now() < deadline);
  struct stat actual {};
  growth::writeFailed = written != 1 || __real_fstat(growth::writer, &actual) != 0;
  growth::after = actual.st_size;
  return result;  // 原真实stat不变，随后配置读取走未包装的真实read。
}

namespace hp::base {
struct AsyncLoggerTestAccess {
  static std::unique_ptr<AsyncLogger> create(std::ostream& sink) {
    return std::unique_ptr<AsyncLogger>(new AsyncLogger(2, sink));
  }
};
}  // namespace hp::base

namespace {
using namespace hp;

void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

std::string request(std::uint16_t port, std::string_view path) {
  net::Socket socket(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
  timeval timeout{5, 0};
  ::setsockopt(socket.fd(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  ::setsockopt(socket.fd(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  check(::connect(socket.fd(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
        "management connect");
  const auto text =
      "GET " + std::string(path) + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  check(::send(socket.fd(), text.data(), text.size(), MSG_NOSIGNAL) ==
            static_cast<ssize_t>(text.size()),
        "management send");
  std::string result;
  char block[4096];
  for (;;) {
    const auto bytes = ::recv(socket.fd(), block, sizeof(block), 0);
    check(bytes >= 0, "management receive");
    if (!bytes) break;
    result.append(block, bytes);
    check(result.size() < 65536, "management response bound");
  }
  return result;
}

void normalManagement(base::LoggerSession& logging) {
  metrics::ServerMetrics metrics;
  base::ResourceBudget budget;
  std::atomic<app::Lifecycle> state{app::Lifecycle::kStarting};
  app::ManagementServer server(0, state, metrics, budget, logging);
  const auto port = server.start();
  check(request(port, "/readyz").find("503") != std::string::npos, "starting not ready");
  state = app::Lifecycle::kReady;
  check(request(port, "/readyz").find("200") != std::string::npos, "ready");
  state = app::Lifecycle::kStopping;
  check(request(port, "/readyz").find("503") != std::string::npos, "stopping not ready");
  check(request(port, "/metrics").find("requests_started_total 0") != std::string::npos,
        "admin must not count itself");
  std::thread stopper(&app::ManagementServer::requestStop, &server);
  server.requestStop();
  stopper.join();
  server.join();
  check(!server.failed() && budget.snapshot().connections.current == 0,
        "normal management cleanup");
  std::cout << "CASE OPS normal Starting/Ready/Stopping/stop race PASS\n";
}

void onManagementFailure(std::atomic<unsigned>& notices, net::TcpServer& business) {
  ++notices;
  business.requestServerStop();
}

void startInjectedManagement(app::ManagementServer& server,
                             injection::Mode mode,
                             std::exception_ptr& error,
                             std::chrono::steady_clock::time_point& startedAt) {
  startedAt = std::chrono::steady_clock::now();
  injection::arm(mode);
  try {
    server.start();
  } catch (...) {
    error = std::current_exception();
  }
}

std::size_t openFdCount() {
  return static_cast<std::size_t>(
      std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                    std::filesystem::directory_iterator{}));
}

void injectedManagement(base::LoggerSession& logging, injection::Mode mode) {
  metrics::ServerMetrics metrics;
  base::ResourceBudget budget;
  std::atomic<app::Lifecycle> state{app::Lifecycle::kStarting};
  net::TcpServer business(0);
  app::ManagementServer server(0, state, metrics, budget, logging);
  std::atomic<unsigned> notices{};
  server.setFailedCallback([&] { onManagementFailure(notices, business); });
  const auto beforeFds = openFdCount();
  std::exception_ptr error;
  std::thread starter;
  try {
    if (mode == injection::Mode::kTimeout) {
      std::chrono::steady_clock::time_point startedAt;
      starter = std::thread([&] { startInjectedManagement(server, mode, error, startedAt); });
      injection::awaitEntry();
      {
        std::unique_lock lock(injection::mutex);
        injection::changed.wait_until(lock, startedAt + 2s, [] { return false; });
      }
      injection::release();
      starter.join();
      check(error != nullptr, "timeout must fail");
      try {
        std::rethrow_exception(error);
      } catch (const std::exception& failure) {
        check(std::string(failure.what()).find("timeout") != std::string::npos,
              "timeout error identity");
      }
    } else {
      injection::arm(mode);
      try {
        server.start();
      } catch (...) {
        error = std::current_exception();
      }
      if (mode == injection::Mode::kRun) {
        check(!error, "runtime must start successfully");
        injection::awaitEntry();
        injection::release();
        server.join();
        check(server.failed() && state == app::Lifecycle::kFailed && notices == 1,
              "runtime failure notification");
      } else
        check(error != nullptr, "startup must fail");
    }
    injection::release();
    server.requestStop();
    server.join();
    check(openFdCount() == beforeFds, "management fd cleanup");
    check(injection::createHits == 1 &&
              injection::epollHits == (mode == injection::Mode::kCreate ? 0U : 1U) &&
              !injection::gateExpired,
          "exact injection hit");
    check(injection::targetExits == (mode == injection::Mode::kCreate ? 0U : 1U),
          "target TLS cleared and exited");
    std::cout << "CASE OPS armed caller=" << injection::caller
              << " target=" << injection::targetThread << " exits=" << injection::targetExits
              << "\n";
    std::cout << "CASE OPS injection " << static_cast<int>(mode)
              << " create_hits=1 epoll_hits=" << injection::epollHits << " clear+join PASS\n";
  } catch (...) {
    injection::release();
    if (starter.joinable()) starter.join();
    server.requestStop();
    server.join();
    throw;
  }
  const auto previousHits = injection::createHits.load();
  normalManagement(logging);
  check(injection::createHits == previousHits && !injection::armed,
        "unarmed normal control untouched");
}

class BlockingSink final : public std::streambuf {
 public:
  void waitEntered();
  void release();
  bool expired() const { return expired_; }

 protected:
  std::streamsize xsputn(const char*, std::streamsize length) override;

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool entered_{false}, released_{false};
  std::atomic<bool> expired_{false};
};

std::streamsize BlockingSink::xsputn(const char*, std::streamsize length) {
  std::unique_lock lock(mutex_);
  entered_ = true;
  changed_.notify_all();
  if (!changed_.wait_for(lock, 5s, [this] { return released_; })) expired_ = true;
  return length;
}

void BlockingSink::waitEntered() {
  std::unique_lock lock(mutex_);
  check(changed_.wait_for(lock, 5s, [this] { return entered_; }), "sink not entered");
}

void BlockingSink::release() {
  std::lock_guard lock(mutex_);
  released_ = true;
  changed_.notify_all();
}

void stopLogger(base::AsyncLogger& logger,
                std::promise<void>& entering,
                std::atomic<bool>& returned) {
  entering.set_value();
  logger.stopAsyncLogging();
  returned = true;
}

class BlockedBusiness final : public business::TaskWork {
 public:
  explicit BlockedBusiness(std::shared_future<void> release) : release_(std::move(release)) {}
  void executeTask(const business::TaskCancellation&) override;
  void waitEntered();
  bool expired() const { return expired_; }

 private:
  std::shared_future<void> release_;
  std::promise<void> entered_;
  std::atomic<bool> expired_{};
};

void BlockedBusiness::executeTask(const business::TaskCancellation&) {
  entered_.set_value();
  if (release_.wait_for(5s) != std::future_status::ready) expired_ = true;
}

void BlockedBusiness::waitEntered() {
  check(entered_.get_future().wait_for(5s) == std::future_status::ready, "business work entry");
}

void testBlockedBusiness(base::LoggerSession& logging) {
  business::BoundedTaskExecutor executor;
  std::promise<void> release;
  auto future = release.get_future().share();
  auto first = std::make_shared<BlockedBusiness>(future);
  auto second = std::make_shared<BlockedBusiness>(future);
  bool released = false;
  try {
    check(executor.submitTask(first).admission == business::TaskAdmission::kAccepted,
          "first work admission");
    check(executor.submitTask(second).admission == business::TaskAdmission::kAccepted,
          "second work admission");
    first->waitEntered();
    second->waitEntered();
    check(executor.snapshot().running == 2, "business workers blocked");
    normalManagement(logging);
    check(executor.snapshot().running == 2, "management must not wait for business workers");
    release.set_value();
    released = true;
    executor.stopAcceptingAndCancel();
    executor.joinWorkers();
    check(!first->expired() && !second->expired() && executor.snapshot().outstanding == 0,
          "business cooperative cleanup");
  } catch (...) {
    if (!released) release.set_value();
    executor.stopAcceptingAndCancel();
    executor.joinWorkers();
    throw;
  }
  std::cout << "CASE OPS management closes before blocked business workers are released PASS\n";
}

void testSlowSink() {
  BlockingSink buffer;
  std::ostream stream(&buffer);
  auto logger = base::AsyncLoggerTestAccess::create(stream);
  std::thread stopping;
  std::promise<void> enteringStop;
  std::atomic<bool> returned{};
  try {
    logger->submitLogRecord(base::LogLevel::kInfo, "blocked");
    buffer.waitEntered();
    for (unsigned i = 0; i < 100; ++i) logger->submitLogRecord(base::LogLevel::kInfo, "bounded");
    check(logger->stats().droppedFull > 0, "slow sink bounded drops");
    stopping = std::thread([&] { stopLogger(*logger, enteringStop, returned); });
    check(enteringStop.get_future().wait_for(5s) == std::future_status::ready && !returned,
          "blocked sink stop boundary");
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (logger->stats().rejectedStopped == 0) {
      logger->submitLogRecord(base::LogLevel::kInfo, "stop-state probe");
      check(std::chrono::steady_clock::now() < deadline, "stop not entered");
      std::this_thread::yield();
    }
    check(!returned, "stop must still await blocked sink");
    buffer.release();
    stopping.join();
    const auto value = logger->stats();
    check(returned && !buffer.expired() && value.pending == 0 &&
              value.accepted == value.written + value.failed,
          "sink final conservation");
  } catch (...) {
    buffer.release();
    if (stopping.joinable()) stopping.join();
    throw;
  }
  std::cout << "CASE OPS real AsyncLogger blocked sink/stats/drop/join PASS\n";
}

void testConfigGrowth(const std::filesystem::path& root) {
  const auto file = root / "operations-growing.json";
  std::string text = "{\"port\":0,\"root\":\".\"}";
  text.resize(65536, ' ');
  {
    std::ofstream output(file);
    output << text;
  }
  std::array<std::string, 3> words{"server", "--config", file.string()};
  std::array<char*, 3> argv{words[0].data(), words[1].data(), words[2].data()};
  net::Socket writer(::open(file.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC));
  check(writer.valid(), "growth writer open");
  try {
    check(app::parseServerOptions(argv.size(), argv.data()).root == ".",
          "valid 65536 normal control");
    struct stat identity {};
    check(::fstat(writer.fd(), &identity) == 0 && identity.st_size == 65536,
          "growth original identity");
    growth::device = identity.st_dev;
    growth::inode = identity.st_ino;
    growth::writer = writer.fd();
    growth::caller = pthread_self();
    growth::hits = 0;
    growth::writeFailed = false;
    growth::armed = true;
    bool rejected = false;
    try {
      (void)app::parseServerOptions(argv.size(), argv.data());
    } catch (const std::invalid_argument& error) {
      rejected = std::string(error.what()).find("grew beyond 64KiB") != std::string::npos;
    }
    growth::armed = false;
    check(rejected && growth::hits == 1 && !growth::writeFailed && growth::before == 65536 &&
              growth::after == 65537,
          "actual post-fstat growth rejection");
    check(::fcntl(growth::observedReader, F_GETFD) == -1 && errno == EBADF,
          "growth read fd closed");
    std::cout << "CASE OPS growth dev=" << growth::device << " inode=" << growth::inode
              << " reader=" << growth::observedReader << " writer=" << growth::writer
              << " before=65536 after=65537 hits=1 PASS\n";
    writer.reset();
    std::filesystem::remove(file);
  } catch (...) {
    growth::armed = false;
    writer.reset();
    std::filesystem::remove(file);
    throw;
  }
}

void testConfig() {
  const auto root = std::filesystem::path(std::getenv("HP_S3_TEST_TMP_ROOT"));
  std::filesystem::create_directories(root);
  const auto file = root / "operations-config.json";
  {
    std::ofstream output(file);
    output << "{\"port\":0,\"root\":\".\",\"threads\":3,\"access_log\":true}";
  }
  std::string path = file.string();
  std::array<std::string, 7>
      words{"server", "--threads", "0", "--config", path, "--no-access-log", "--admin-port"};
  std::vector<std::string> arguments(words.begin(), words.end());
  arguments.push_back("0");
  std::vector<char*> argv;
  for (auto& word : arguments) argv.push_back(word.data());
  const auto options = app::parseServerOptions(argv.size(), argv.data());
  check(options.threads == 0 && !options.accessLog && options.adminPort == 0, "config priority");
  std::filesystem::remove(file);
  testConfigGrowth(root);
  if (std::getenv("HP_OPERATIONS_NEGATIVE")) requireTestCondition(false);
  net::Socket listener = net::Socket::createTcp();
  listener.bindIpv4(0, net::ListenAddress::kLoopback);
  sockaddr_in bound{};
  socklen_t size = sizeof(bound);
  check(::getsockname(listener.fd(), reinterpret_cast<sockaddr*>(&bound), &size) == 0 &&
            bound.sin_addr.s_addr == htonl(INADDR_LOOPBACK),
        "actual loopback bind");
  std::cout << "CASE OPS config priority/actual loopback bind PASS\n";
}
}  // namespace

int main() {
  injection::nextCreate = reinterpret_cast<injection::Create>(dlsym(RTLD_NEXT, "pthread_create"));
  check(injection::nextCreate != nullptr && injection::nextCreate != &pthread_create,
        "pthread next resolution");
  Dl_info symbol{};
  check(dladdr(reinterpret_cast<void*>(injection::nextCreate), &symbol) != 0,
        "pthread next source");
  std::cout << "CASE OPS pthread next source " << symbol.dli_fname << '\n';
  testConfig();
  base::LoggerSession logging;
  normalManagement(logging);
  for (auto mode : {injection::Mode::kCreate,
                    injection::Mode::kInitialize,
                    injection::Mode::kTimeout,
                    injection::Mode::kRun})
    injectedManagement(logging, mode);
  testBlockedBusiness(logging);
  testSlowSink();
}
