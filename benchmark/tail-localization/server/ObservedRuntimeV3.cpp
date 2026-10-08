#include "StartupEvidence.h"
#include "ObservedRuntime.h"
#include "ObserverBuffer.h"
#include "ClockEvidence.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <netinet/in.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

namespace tail_localization {
namespace {
HpS4Control* control{nullptr};
std::array<std::unique_ptr<ObserverBuffer>, HP_S4_WORKERS> buffers;
std::atomic<std::uint32_t> nextConnection{0};
thread_local std::uint32_t worker{UINT32_MAX};
thread_local ConnectionContext* currentConnection{nullptr};
std::string outputDirectory;

std::uint64_t monotonicNs() {
  timespec value{};
  if (::clock_gettime(CLOCK_MONOTONIC, &value)) throw std::runtime_error("observer clock failed");
  return value.tv_sec * 1000000000ULL + value.tv_nsec;
}

std::uint64_t threadStarttime(std::uint32_t tid) {
  std::ifstream input("/proc/self/task/" + std::to_string(tid) + "/stat");
  std::string line;
  std::getline(input, line);
  const auto close = line.rfind(')');
  if (close == std::string::npos) throw std::runtime_error("observer identity missing");
  std::istringstream fields(line.substr(close + 2));
  std::string field;
  for (int index = 3; index <= 22; ++index) if (!(fields >> field)) throw std::runtime_error("short stat");
  return std::stoull(field);
}

void registerThread(HpS4Thread& identity, std::uint32_t index, const char* role) {
  identity.pid = ::getpid();
  identity.tid = ::syscall(SYS_gettid);
  identity.worker = index;
  identity.starttime = threadStarttime(identity.tid);
  const char* marker = std::getenv("HP_S4_MARKER_FD");
  if (marker) {
    char message[192];
    const int size = std::snprintf(message, sizeof(message), "S4MARK server %s %u %u %u %llu\n", role,
                                   index, identity.pid, identity.tid,
                                   static_cast<unsigned long long>(identity.starttime));
    if (size <= 0 || ::write(std::stoi(marker), message, size) != size)
      throw std::runtime_error("observer marker failed");
  }
  __atomic_store_n(&identity.ready, 1U, __ATOMIC_RELEASE);
}

bool selected(std::uint32_t index) noexcept {
  if (!control || !__atomic_load_n(&control->go, __ATOMIC_ACQUIRE)) return false;
  for (std::uint32_t item = 0; item < control->selectedCount; ++item)
    if (control->selectedServer[item] == index) return true;
  return false;
}
}  // namespace

void initializeObserver() {
  const char* path = std::getenv("HP_S4_CONTROL");
  if (!path) return;
  const char* output = std::getenv("HP_S4_OUTPUT");
  if (!output) throw std::runtime_error("observer output missing");
  outputDirectory = output;
  if (saveClockEvidence(output,"server")) throw std::runtime_error("observer clock evidence failed");
  const int fd = ::open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
  struct stat status{};
  if (fd < 0 || ::fstat(fd, &status) || status.st_size != sizeof(HpS4Control)) {
    if (fd >= 0) ::close(fd);
    throw std::runtime_error("observer control identity invalid");
  }
  void* mapping = ::mmap(nullptr, sizeof(HpS4Control), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);
  if (mapping == MAP_FAILED) throw std::runtime_error("observer control mmap failed");
  control = static_cast<HpS4Control*>(mapping);
  if (std::memcmp(control->magic, "S4CTRL03", 8) || control->version != 3 ||
      control->bytes != sizeof(HpS4Control) || control->connections > HP_S4_MAX_CONNECTIONS) {
    ::munmap(control, sizeof(HpS4Control));
    control = nullptr;
    throw std::runtime_error("observer control schema invalid");
  }
  for (auto& buffer : buffers) {
    buffer = std::make_unique<ObserverBuffer>(HP_S4_BUFFER_BYTES);
    buffer->setEnabledBeforeCallbacks(control->detailed != 0);
  }
  registerThread(control->serverThreads[4], 4, "main");
}

void closeMarkerAfterStartup() {
  if (!control) return;
  const auto deadline = monotonicNs() + 1000000000ULL;
  for (;;) {
    bool complete = true;
    for (const auto& identity : control->serverThreads)
      if (!__atomic_load_n(&identity.ready, __ATOMIC_ACQUIRE)) complete = false;
    if (complete) break;
    if (monotonicNs() >= deadline) throw std::runtime_error("observer startup threads missing");
    ::usleep(1000);
  }
  const char* marker = std::getenv("HP_S4_MARKER_FD");
  if (marker && ::close(std::stoi(marker))) throw std::runtime_error("observer marker close failed");
  ::unsetenv("HP_S4_MARKER_FD");
}

void registerWorker(std::size_t workerIndex) {
  if (!control) return;
  const auto triggerPhase = __atomic_load_n(&control->phase, __ATOMIC_ACQUIRE);
  if (workerIndex >= buffers.size()) {
    HpS4Connection attempted{};
    attempted.worker = workerIndex;
    attempted.pid = ::getpid();
    attempted.tid = ::syscall(SYS_gettid);
    attempted.starttime = threadStarttime(attempted.tid);
    attempted.fd = -1;
    publishStartupFailure(control, 0, HP_S4_WORKER_RANGE, &attempted, 0, triggerPhase);
    throw std::runtime_error("workerIndexOutOfRange");
  }
  worker = workerIndex;
  registerThread(control->serverThreads[workerIndex], workerIndex, "worker");
}

void registerLoggerThread() {
  if (control) registerThread(control->serverThreads[5], 5, "logger");
}

void stopWorker(std::size_t workerIndex) noexcept {
  if (!control) return;
  try { control->serverThreads[workerIndex].stoppedNs = monotonicNs(); }
  catch (...) { __atomic_store_n(&control->abortRun, 1U, __ATOMIC_RELEASE); }
}

ConnectionContext registerConnection(int fd, std::uint64_t lifetime) {
  ConnectionContext context;
  if (!control) return context;
  const auto triggerPhase = __atomic_load_n(&control->phase, __ATOMIC_ACQUIRE);
  const auto index = nextConnection.fetch_add(1, std::memory_order_relaxed);
  __atomic_add_fetch(&control->serverAttempts, 1U, __ATOMIC_RELAXED);
  HpS4Connection attempted{};
  attempted.worker = worker;
  attempted.pid = ::getpid();
  attempted.tid = ::syscall(SYS_gettid);
  attempted.starttime = threadStarttime(attempted.tid);
  attempted.fd = fd;
  attempted.index = index;
  attempted.lifetime = lifetime;
  sockaddr_in local{}, remote{};
  socklen_t localBytes = sizeof(local), remoteBytes = sizeof(remote);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &localBytes) ||
      ::getpeername(fd, reinterpret_cast<sockaddr*>(&remote), &remoteBytes) ||
      local.sin_family != AF_INET || remote.sin_family != AF_INET)
  {
    publishStartupFailure(control, 0, HP_S4_TUPLE_FAILURE, &attempted, errno, triggerPhase);
    throw std::runtime_error("observer connection tuple missing");
  }
  attempted.localAddress = local.sin_addr.s_addr;
  attempted.remoteAddress = remote.sin_addr.s_addr;
  attempted.localPort = ntohs(local.sin_port);
  attempted.remotePort = ntohs(remote.sin_port);
  if (worker == UINT32_MAX) {
    publishStartupFailure(control, 0, HP_S4_WORKER_MISSING, &attempted, 0, triggerPhase);
    throw std::runtime_error("workerNotRegistered");
  }
  if (worker >= buffers.size()) {
    publishStartupFailure(control, 0, HP_S4_WORKER_RANGE, &attempted, 0, triggerPhase);
    throw std::runtime_error("workerIndexOutOfRange");
  }
  if (index >= control->connections) {
    publishStartupFailure(control, 0, HP_S4_CONNECTION_LIMIT, &attempted, 0, triggerPhase);
    throw std::runtime_error("connectionLimitExceeded");
  }
  if (__atomic_load_n(&control->phase, __ATOMIC_ACQUIRE) != HP_S4_REGISTERING) {
    publishStartupFailure(control, 0, HP_S4_DUPLICATE_REGISTRATION, &attempted, 0, triggerPhase);
    throw std::runtime_error("registrationOutsidePhase");
  }
  while (__atomic_exchange_n(&control->registrationLock, 1U, __ATOMIC_ACQUIRE)) ::usleep(100);
  bool duplicate = false;
  for (std::uint32_t previous = 0; previous < control->connections; ++previous) {
    const auto& existing = control->serverConnections[previous];
    if (__atomic_load_n(&existing.ready, __ATOMIC_ACQUIRE) &&
        (existing.worker == worker && (existing.lifetime == lifetime || existing.fd == fd)))
      duplicate = true;
  }
  if (duplicate) {
    __atomic_store_n(&control->registrationLock, 0U, __ATOMIC_RELEASE);
    publishStartupFailure(control, 0, HP_S4_DUPLICATE_REGISTRATION, &attempted, 0, triggerPhase);
    throw std::runtime_error("duplicateRegistration");
  }
  auto& record = control->serverConnections[index];
  record.worker = worker;
  record.pid = ::getpid();
  record.tid = ::syscall(SYS_gettid);
  record.starttime = control->serverThreads[worker].starttime;
  record.lifetime = lifetime;
  record.localAddress = local.sin_addr.s_addr;
  record.remoteAddress = remote.sin_addr.s_addr;
  record.localPort = ntohs(local.sin_port);
  record.remotePort = ntohs(remote.sin_port);
  record.fd = fd;
  record.index = index;
  __atomic_store_n(&record.ready, 1U, __ATOMIC_RELEASE);
  const auto registered = __atomic_add_fetch(&control->serverRegistered, 1U, __ATOMIC_RELEASE);
  auto& registration = control->serverRegistrations[index];
  registration.timeNs = monotonicNs();
  registration.phase = HP_S4_REGISTERING;
  registration.expected = control->connections;
  registration.registered = registered;
  registration.index = index;
  registration.endpoint = 0;
  __atomic_store_n(&registration.ready, 1U, __ATOMIC_RELEASE);
  __atomic_store_n(&control->registrationLock, 0U, __ATOMIC_RELEASE);
  context.index = index;
  return context;
}

std::uint32_t prospectiveSequence(const ConnectionContext& context) noexcept {
  return context.sequence + static_cast<std::uint32_t>(context.drained);
}

ConnectionContext* exchangeCurrentConnection(ConnectionContext* context) noexcept {
  auto* previous = currentConnection;
  currentConnection = context;
  return previous;
}

void appendConnectionEvent(ConnectionContext& context, std::uint16_t kind, std::uint64_t value,
                           std::int32_t result, std::uint16_t flags) noexcept {
  if (worker >= buffers.size() || context.index == UINT32_MAX || !selected(context.index)) return;
  const auto sequence = kind == 5 || kind == 6 || kind == 7 ? prospectiveSequence(context) : context.sequence;
  buffers[worker]->appendEvent(static_cast<EventKind>(kind), context.index, sequence, value, result, flags);
  if (buffers[worker]->overflowWhileWriterActive() || buffers[worker]->invalidWhileWriterActive())
    __atomic_store_n(&control->abortRun, 1U, __ATOMIC_RELEASE);
}

void appendIoEvent(std::uint16_t kind, std::uint64_t value, std::int32_t result,
                   std::uint16_t flags) noexcept {
  if (currentConnection) appendConnectionEvent(*currentConnection, kind, value, result, flags);
}

void receiveSucceeded() noexcept {
  const auto triggerPhase = control ? __atomic_load_n(&control->phase, __ATOMIC_ACQUIRE) : HP_S4_INITIALIZING;
  if (control && currentConnection && !__atomic_load_n(&control->go, __ATOMIC_ACQUIRE)) {
    publishStartupFailure(control, 0, HP_S4_REQUEST_BEFORE_GO,
                          &control->serverConnections[currentConnection->index], 0, triggerPhase);
    return;
  }
  if (currentConnection && currentConnection->drained) {
    ++currentConnection->sequence;
    currentConnection->drained = false;
  }
}

void markOutputDrained(ConnectionContext& context) noexcept {
  appendConnectionEvent(context, 15);
  context.drained = true;
}

void closeConnection(ConnectionContext& context) noexcept {
  if (!control || context.index == UINT32_MAX) return;
  const auto triggerPhase = __atomic_load_n(&control->phase, __ATOMIC_ACQUIRE);
  if (!__atomic_load_n(&control->go, __ATOMIC_ACQUIRE) &&
      !__atomic_load_n(&control->abortRun, __ATOMIC_ACQUIRE))
    publishStartupFailure(control, 0, HP_S4_CLOSED_BEFORE_GO,
                          &control->serverConnections[context.index], 0, triggerPhase);
  appendConnectionEvent(context, 23);
  control->serverConnections[context.index].reserved = context.sequence | (static_cast<std::uint64_t>(context.drained) << 32);
  __atomic_store_n(&control->serverConnections[context.index].closed, 1U, __ATOMIC_RELEASE);
}

void exportObserverAfterWorkersJoined() {
  if (!control) return;
  for (std::size_t index = 0; index < buffers.size(); ++index) {
    if (!control->serverThreads[index].ready || !control->serverThreads[index].stoppedNs)
      throw std::runtime_error("observer writer did not stop");
    const auto& buffer = *buffers[index];
    const auto count = buffer.countAfterWriterStopped();
    HpS4Header header{};
    std::memcpy(header.magic, "S4TAIL02", 8);
    header.headerBytes = sizeof(header);
    header.eventBytes = sizeof(HpS4Event);
    header.flags = 1 | (buffer.overflowWhileWriterActive() ? 2 : 0) | (buffer.invalidWhileWriterActive() ? 4 : 0);
    header.count = count;
    header.capacity = HP_S4_BUFFER_BYTES / sizeof(HpS4Event);
    header.pid = control->serverThreads[index].pid;
    header.tid = control->serverThreads[index].tid;
    header.starttime = control->serverThreads[index].starttime;
    const auto* records = buffer.recordsAfterWriterStopped();
    if (count) { header.firstNs = records[0].timeNs; header.lastNs = records[count - 1].timeNs; }
    std::ofstream output(outputDirectory + "/server-worker-" + std::to_string(index) + ".events.bin", std::ios::binary);
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(records), count * sizeof(HpS4Event));
    output.close();
    if (!output) throw std::runtime_error("observer export failed");
  }
  ::munmap(control, sizeof(HpS4Control));
  control = nullptr;
}
}  // namespace tail_localization
