#include "base/ResourceBudget.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <new>
#include <thread>
#include <vector>

#include "HttpConnectionHandler.h"
#include "TestCheck.h"
#include "business/BoundedTaskExecutor.h"
#include "net/ConnectionIo.h"

namespace {
std::atomic<bool> failNextAllocation{false};
}

void* operator new(std::size_t bytes) {
  if (failNextAllocation.exchange(false)) throw std::bad_alloc{};
  if (auto* allocation = std::malloc(bytes ? bytes : 1)) return allocation;
  throw std::bad_alloc{};
}

void operator delete(void* allocation) noexcept { std::free(allocation); }

void operator delete(void* allocation, std::size_t) noexcept { std::free(allocation); }

namespace hp::net {
struct TcpServerTestAccess {
  static void deliver(TcpServer& server, Socket socket) { server.onAccepted(std::move(socket)); }
  static void stopPool(TcpServer& server) { server.workerPool_.requestPoolStop(); }
  static bool post(TcpServer& server, EventLoopThread::TaskCallback task) {
    return server.workerPool_.postTaskToWorkerAtIndex(0, std::move(task));
  }
};
}  // namespace hp::net

namespace {
using namespace hp::base;
using namespace hp::net;
using namespace std::chrono_literals;

void checkZero(const ResourceBudget& budget) {
  const auto value = budget.snapshot();
  requireTestCondition(value.connections.current == 0 && value.inputBytes.current == 0 &&
                       value.outputBytes.current == 0);
}

void testLimits() {
  ResourceBudget budget;
  std::vector<ResourceLease> connections;
  for (unsigned i = 0; i < 128; ++i) {
    auto acquired = budget.tryAcquireConnection();
    requireTestCondition(acquired.accepted());
    connections.push_back(std::move(acquired.lease));
  }
  requireTestCondition(budget.tryAcquireConnection().rejection == ResourceRejection::kConnections);
  std::vector<ResourceLease> requests;
  for (unsigned i = 0; i < 61; ++i) {
    auto acquired = budget.tryAcquireInput(kRequestInputBytes);
    requireTestCondition(acquired.accepted());
    requests.push_back(std::move(acquired.lease));
  }
  requireTestCondition(budget.tryAcquireInput(kRequestInputBytes).rejection ==
                       ResourceRejection::kInputBytes);
  requireTestCondition(budget.snapshot().inputBytes.current ==
                       128 * kTransportInputBytes + 61 * kRequestInputBytes);
  auto fullOutput = budget.tryAcquireOutput(64U * 1024U * 1024U);
  requireTestCondition(fullOutput.accepted() && !budget.tryAcquireOutput(1).accepted());
  fullOutput.lease.reset();
  requests.clear();
  connections.clear();
  checkZero(budget);
  ResourceBudget noWindow({4, kTransportInputBytes - 1, 0});
  requireTestCondition(noWindow.tryAcquireConnection().rejection == ResourceRejection::kInputBytes);
  checkZero(noWindow);
  const auto maximum = std::numeric_limits<std::size_t>::max();
  ResourceBudget extreme({maximum, maximum, maximum});
  auto input = extreme.tryAcquireInput(maximum);
  auto output = extreme.tryAcquireOutput(maximum);
  requireTestCondition(input.accepted() && output.accepted());
  requireTestCondition(!extreme.tryAcquireInput(1).accepted() &&
                       !extreme.tryAcquireOutput(1).accepted());
  ResourceLease moved(std::move(input.lease));
  requireTestCondition(input.lease.empty());
  moved = std::move(output.lease);  // 先归还原输入，再接管输出。
  requireTestCondition(extreme.snapshot().inputBytes.current == 0);
  moved.releaseOutput(maximum);
  moved.reset();
  checkZero(extreme);
  requireTestCondition(std::getenv("HP_RESOURCE_NEGATIVE") == nullptr);
  std::cout << "CASE R01 defaults 128/61, atomic admission, overflow and move PASS\n";
}

class ConcurrentBudgetProbe {
 public:
  void runProducer();
  void verifyFinal() const;

 private:
  ResourceBudget budget_{{4, 4 * kTransportInputBytes + 2 * kRequestInputBytes, 4096}};
  std::barrier<> start_{8};
};

void ConcurrentBudgetProbe::runProducer() {
  start_.arrive_and_wait();
  for (unsigned i = 0; i < 256; ++i) {
    auto connection = budget_.tryAcquireConnection();
    auto request = budget_.tryAcquireInput(kRequestInputBytes);
    auto output = budget_.tryAcquireOutput(1024);
    ResourceLease transferred = std::move(connection.lease);
    if (output.accepted()) output.lease.releaseOutput(512);
    const auto value = budget_.snapshot();
    requireTestCondition(value.connections.current <= 4 &&
                         value.inputBytes.current <=
                             4 * kTransportInputBytes + 2 * kRequestInputBytes &&
                         value.outputBytes.current <= 4096);
  }
}

void ConcurrentBudgetProbe::verifyFinal() const { checkZero(budget_); }

void testConcurrency() {
  ConcurrentBudgetProbe probe;
  std::vector<std::jthread> producers;
  for (unsigned i = 0; i < 8; ++i)
    producers.emplace_back(&ConcurrentBudgetProbe::runProducer, &probe);
  for (auto& producer : producers) producer.join();
  probe.verifyFinal();
  std::cout << "CASE R02 8x256 acquire/move/partial-release PASS\n";
}

std::array<Socket, 2> socketPair() {
  int descriptors[2];
  requireTestCondition(
      ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, descriptors) == 0);
  int buffer = 4096;
  requireTestCondition(
      ::setsockopt(descriptors[0], SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer)) == 0);
  return {Socket(descriptors[0]), Socket(descriptors[1])};
}

FileRegion temporaryFile(std::size_t size) {
  const auto* temp = std::getenv("HP_S3_TEST_TMP_ROOT");
  requireTestCondition(temp != nullptr);
  std::filesystem::create_directories(temp);
  auto path = (std::filesystem::path(temp) / "resource-file-XXXXXX").string();
  UniqueFd descriptor(::mkstemp(path.data()));
  requireTestCondition(descriptor.get() >= 0);
  requireTestCondition(::unlink(path.c_str()) == 0 &&
                       ::ftruncate(descriptor.get(), static_cast<off_t>(size)) == 0);
  return FileRegion(std::move(descriptor), 0, size);
}

void drainOutput(ConnectionIo& io, Socket& peer, const ResourceBudget& budget) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  std::array<std::byte, 16384> buffer;
  while (io.hasPendingOutput()) {
    while (::recv(peer.fd(), buffer.data(), buffer.size(), 0) > 0) {
    }
    const auto before = io.pendingBytes();
    const auto write = io.writeAvailable();
    requireTestCondition(write.errorNumber == 0 &&
                         before - io.pendingBytes() == write.bytesWritten);
    requireTestCondition(budget.snapshot().outputBytes.current == io.pendingBytes());
    requireTestCondition(std::chrono::steady_clock::now() < deadline);
  }
}

void testOutput() {
  auto budget = std::make_shared<ResourceBudget>(
      ResourceLimits{8, 8 * kTransportInputBytes, 2U * 1024U * 1024U});
  {
    auto sockets = socketPair();
    auto connection = budget->tryAcquireConnection();
    ConnectionIo io(std::move(sockets[0]),
                    kTransportInputBytes,
                    budget,
                    std::move(connection.lease));
    std::vector<std::byte> body(1024U * 1024U);
    io.queueOutput(body);
    const auto first = io.writeAvailable();
    requireTestCondition(first.wouldBlock && first.bytesWritten > 0 &&
                         first.bytesWritten < body.size());
    requireTestCondition(budget->snapshot().outputBytes.current ==
                         body.size() - first.bytesWritten);
    auto otherPair = socketPair();
    ConnectionIo other(std::move(otherPair[0]), kTransportInputBytes, budget);
    bool rejected = false;
    try {
      other.queueOutput(std::vector<std::byte>(2U * 1024U * 1024U));
    } catch (const ResourceLimitError&) {
      rejected = true;
    }
    requireTestCondition(rejected && other.pendingBytes() == 0);
    drainOutput(io, sockets[1], *budget);
    other.queueOutput(body);
    io = std::move(other);  // old socket/connection lease released, incoming pending preserved.
    requireTestCondition(budget->snapshot().connections.current == 0 &&
                         budget->snapshot().outputBytes.current == body.size());
  }
  checkZero(*budget);
  {
    auto sockets = socketPair();
    ConnectionIo io(std::move(sockets[0]), kTransportInputBytes, budget);
    auto file = temporaryFile(1024U * 1024U);
    const auto fileFd = file.fd();
    const std::array<std::byte, 16> header{};
    io.queueFile(header, std::move(file));
    const auto first = io.writeAvailable();
    requireTestCondition(first.wouldBlock && first.fileTransfer &&
                         first.bytesWritten > header.size());
    requireTestCondition(budget->snapshot().outputBytes.current ==
                         1024U * 1024U + header.size() - first.bytesWritten);
    drainOutput(io, sockets[1], *budget);
    requireTestCondition(::fcntl(fileFd, F_GETFD) == -1 && errno == EBADF);
  }
  checkZero(*budget);
  // 新缓冲分配实际抛bad_alloc；完整incoming预留必须自动回滚。
  auto sockets = socketPair();
  ConnectionIo io(std::move(sockets[0]), kTransportInputBytes, budget);
  std::array<std::byte, 128> bytes{};
  failNextAllocation = true;
  bool failed = false;
  try {
    io.queueOutput(bytes);
  } catch (const std::bad_alloc&) {
    failed = true;
  }
  failNextAllocation = false;
  requireTestCondition(failed && io.pendingBytes() == 0);
  checkZero(*budget);
  auto file = temporaryFile(1024);
  const auto fileFd = file.fd();
  failNextAllocation = true;
  failed = false;
  try {
    io.queueFile(bytes, std::move(file));
  } catch (const std::bad_alloc&) {
    failed = true;
  }
  failNextAllocation = false;
  requireTestCondition(failed && ::fcntl(fileFd, F_GETFD) == -1 && errno == EBADF);
  checkZero(*budget);
  std::cout << "CASE R03 memory/file actual EAGAIN/progress/move/bad_alloc rollback PASS\n";
}

TcpConnection::MessageCallback throwingFactory() { throw std::runtime_error("injected factory"); }

struct WorkerGate {
  std::promise<void> entered;
  std::promise<void> released;
  void block(EventLoop&) {
    entered.set_value();
    requireTestCondition(released.get_future().wait_for(5s) == std::future_status::ready);
  }
};

void testFailedOwnership() {
  auto budget = std::make_shared<ResourceBudget>();
  {
    EventLoop loop;
    auto sockets = socketPair();
    const auto fd = sockets[0].fd();
    auto acquired = budget->tryAcquireConnection();
    bool failed = false;
    try {
      TcpConnection connection(loop,
                               std::move(sockets[0]),
                               0,
                               kTransportInputBytes,
                               budget,
                               std::move(acquired.lease));
    } catch (const std::invalid_argument&) {
      failed = true;
    }
    requireTestCondition(failed && ::fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    checkZero(*budget);
    ConnectionRegistry registry(loop, kTransportInputBytes, {}, nullptr, budget);
    auto regular = temporaryFile(0);
    const auto duplicate = ::dup(regular.fd());
    acquired = budget->tryAcquireConnection();
    failed = false;
    try {
      registry.onAccepted(Socket(duplicate), {}, std::move(acquired.lease));
    } catch (const std::system_error&) {
      failed = true;
    }
    requireTestCondition(failed && ::fcntl(duplicate, F_GETFD) == -1 && errno == EBADF);
    checkZero(*budget);
  }
  {
    TcpServer server(0, kTransportInputBytes, 0, {}, nullptr, budget);
    server.registerMessageFactoryCallback(throwingFactory);
    auto sockets = socketPair();
    const auto fd = sockets[0].fd();
    bool failed = false;
    try {
      TcpServerTestAccess::deliver(server, std::move(sockets[0]));
    } catch (const std::runtime_error&) {
      failed = true;
    }
    requireTestCondition(failed && ::fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    checkZero(*budget);
  }
  {
    TcpServer server(0, kTransportInputBytes, 1, {}, nullptr, budget);
    auto gate = std::make_shared<WorkerGate>();
    requireTestCondition(
        TcpServerTestAccess::post(server, [gate](EventLoop& loop) { gate->block(loop); }));
    requireTestCondition(gate->entered.get_future().wait_for(5s) == std::future_status::ready);
    TcpServerTestAccess::stopPool(server);
    auto sockets = socketPair();
    const auto fd = sockets[0].fd();
    TcpServerTestAccess::deliver(server, std::move(sockets[0]));
    requireTestCondition(::fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    checkZero(*budget);
    gate->released.set_value();
  }
  checkZero(*budget);
  std::cout << "CASE R04 constructor/epoll-registration/factory/handoff rejection releases "
               "FD+lease PASS\n";
}

class IndependentWorkGate final : public hp::business::TaskWork {
 public:
  IndependentWorkGate() : releaseFuture_(release_.get_future()) {}
  void executeTask(const hp::business::TaskCancellation&) override {
    entered_.set_value();
    if (releaseFuture_.wait_for(5s) != std::future_status::ready) timedOut_ = true;
  }
  void waitEntered() {
    if (entered_.get_future().wait_for(5s) != std::future_status::ready)
      throw std::runtime_error("independent gate entry deadline");
  }
  void release() {
    if (!released_) {
      released_ = true;
      release_.set_value();
    }
  }
  bool timedOut() const { return timedOut_; }

 private:
  std::promise<void> entered_;
  std::promise<void> release_;
  std::future<void> releaseFuture_;
  std::atomic<bool> timedOut_{};
  bool released_{};
};

class SyntheticWork final : public hp::business::TaskWork {
 public:
  void executeTask(const hp::business::TaskCancellation&) override {}
};

hp::business::TaskExecutorSnapshot waitExecutorState(
    const hp::business::BoundedTaskExecutor& executor,
    std::size_t ready,
    std::size_t running) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  for (;;) {
    const auto value = executor.snapshot();
    if (value.ready == ready && value.running == running && value.queued == 0 &&
        value.outstanding == ready + running) {
      if (value.admitted != value.released + value.outstanding)
        throw std::runtime_error("executor conservation");
      return value;
    }
    if (std::chrono::steady_clock::now() >= deadline)
      throw std::runtime_error("executor state deadline");
    std::this_thread::yield();
  }
}

void testCancelledRunningRetirement() {
  using namespace hp::business;
  BoundedTaskExecutor executor({2, 32, 5000ms});
  auto cancelled = std::make_shared<IndependentWorkGate>();
  auto continuing = std::make_shared<IndependentWorkGate>();
  std::array<std::shared_ptr<TaskJob>, 34> jobs;
  // 任何异常先释放两个独立门闩，再停止并 join；不得让析构等待未放行工作。
  auto cleanup = [&] {
    cancelled->release();
    continuing->release();
    executor.stopAcceptingAndCancel();
    executor.joinWorkers();
  };
  try {
    jobs[0] = executor.submitTask(cancelled).job;
    jobs[1] = executor.submitTask(continuing).job;
    cancelled->waitEntered();
    continuing->waitEntered();
    for (unsigned i = 2; i < jobs.size(); ++i) {
      auto submission = executor.submitTask(std::make_shared<SyntheticWork>());
      if (submission.admission != TaskAdmission::kAccepted)
        throw std::runtime_error("initial queue admission");
      jobs[i] = std::move(submission.job);
    }
    executor.cancelTask(jobs[2]);
    executor.cancelTask(jobs[0]);
    continuing->release();
    const auto intermediate = waitExecutorState(executor, 32, 1);
    if (intermediate.outstanding != 33 || intermediate.released != 1 || cancelled->timedOut())
      throw std::runtime_error("cancelled running must retain slot");
    std::cout << "CASE RW02 controlled ready=32 running=1 queued=0 outstanding=33 released=1\n";
    cancelled->release();
    const auto retired = waitExecutorState(executor, 32, 0);
    if (retired.released != 2 || cancelled->timedOut() || continuing->timedOut() ||
        executor.collectTask(jobs[0]).state != TaskJobState::kRetired ||
        executor.collectTask(jobs[2]).state != TaskJobState::kRetired)
      throw std::runtime_error("cancelled retirement");
    for (unsigned i : {0U, 2U}) {
      auto submission = executor.submitTask(std::make_shared<SyntheticWork>());
      if (submission.admission != TaskAdmission::kAccepted)
        throw std::runtime_error("recovered slot admission");
      jobs[i] = std::move(submission.job);
    }
    waitExecutorState(executor, 34, 0);
    if (executor.submitTask(std::make_shared<SyntheticWork>()).admission !=
        TaskAdmission::kOverloaded)
      throw std::runtime_error("recovered full capacity");
    for (const auto& job : jobs)
      if (executor.collectTask(job).state != TaskJobState::kReady)
        throw std::runtime_error("collect recovered ready jobs");
    const auto final = executor.snapshot();
    if (final.outstanding != 0 || final.admitted != 36 || final.released != 36)
      throw std::runtime_error("final release conservation");
    cleanup();
  } catch (...) {
    cleanup();
    throw;
  }
  std::cout << "CASE RW02 quiescent32/refill34/final36 released PASS\n";
}
}  // namespace

int main() {
  testLimits();
  testConcurrency();
  testOutput();
  testFailedOwnership();
  testCancelledRunningRetirement();
}
