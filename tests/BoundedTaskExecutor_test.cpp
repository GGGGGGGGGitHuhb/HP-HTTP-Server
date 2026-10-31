#include "business/BoundedTaskExecutor.h"

#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "TestCheck.h"
#include "net/ConnectionRegistry.h"

namespace hp::net {
struct ConnectionTimeoutTestAccess {
  static TcpConnection& connection(ConnectionRegistry& registry) {
    requireTestCondition(registry.connections_.size() == 1);
    return *registry.connections_.begin()->second;
  }
  static void deliver(ConnectionRegistry& registry, int fd, TcpConnection::Identity identity) {
    registry.deliverContinuation(fd, identity);
  }
};
}  // namespace hp::net

namespace {
using namespace hp::business;
using namespace std::chrono_literals;

struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  unsigned entered{};
  bool released{};
  void enter() {
    std::unique_lock lock(mutex);
    ++entered;
    changed.notify_all();
    requireTestCondition(changed.wait_for(lock, 5s, [this] { return released; }));
  }
  void wait(unsigned count) {
    std::unique_lock lock(mutex);
    requireTestCondition(changed.wait_for(lock, 5s, [this, count] { return entered >= count; }));
  }
  void release() {
    std::lock_guard lock(mutex);
    released = true;
    changed.notify_all();
  }
  ~Gate() { release(); }
};
struct Work final : TaskWork {
  std::shared_ptr<Gate> gate;
  std::atomic<unsigned>& executed;
  bool throws{};
  Work(std::atomic<unsigned>& count, std::shared_ptr<Gate> barrier = {}, bool fail = false)
      : gate(std::move(barrier)), executed(count), throws(fail) {}
  void executeTask(const TaskCancellation&) override {
    if (gate) gate->enter();
    ++executed;
    if (throws) throw std::runtime_error("injected work failure");
  }
};
void waitReady(BoundedTaskExecutor& executor, std::size_t ready) {
  const auto limit = std::chrono::steady_clock::now() + 5s;
  while (executor.snapshot().ready != ready) {
    requireTestCondition(std::chrono::steady_clock::now() < limit);
    std::this_thread::yield();
  }
}
void checkConservation(const BoundedTaskExecutor& executor) {
  auto value = executor.snapshot();
  requireTestCondition(value.outstanding == value.queued + value.running + value.ready);
  requireTestCondition(value.admitted == value.released + value.outstanding);
}
struct ContinuationProbe {
  hp::net::EventLoop& loop;
  unsigned calls{};
  void onContinuation(hp::net::TcpConnection& connection) {
    ++calls;
    requireTestCondition(loop.timerCount() == 0);
    connection.scheduleContinuation(
        std::chrono::steady_clock::now() + 1h,
        [this](hp::net::TcpConnection& target) { onContinuation(target); });
  }
};
void testContinuationIdentity() {
  hp::net::EventLoop loop;
  hp::net::ConnectionRegistry registry(loop, 16384);
  int pair[2];
  requireTestCondition(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) ==
                       0);
  hp::net::Socket peer(pair[1]);
  registry.onAccepted(hp::net::Socket(pair[0]), {});
  auto& first = hp::net::ConnectionTimeoutTestAccess::connection(registry);
  const auto fd = first.fd();
  const auto identity = first.identity();
  ContinuationProbe probe{loop};
  first.scheduleContinuation(
      std::chrono::steady_clock::now(),
      [&probe](hp::net::TcpConnection& target) { probe.onContinuation(target); });
  requireTestCondition(loop.timerCount() == 1);
  loop.pollOnce(0);
  requireTestCondition(probe.calls == 1 && loop.timerCount() == 1);
  first.requestClose();
  registry.onCleanup();
  requireTestCondition(loop.timerCount() == 0);
  peer.reset();
  requireTestCondition(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) ==
                       0);
  requireTestCondition(pair[0] == fd);
  peer.reset(pair[1]);
  registry.onAccepted(hp::net::Socket(pair[0]), {});
  auto& second = hp::net::ConnectionTimeoutTestAccess::connection(registry);
  requireTestCondition(second.identity() != identity);
  second.scheduleContinuation(
      std::chrono::steady_clock::now() + 1h,
      [&probe](hp::net::TcpConnection& target) { probe.onContinuation(target); });
  hp::net::ConnectionTimeoutTestAccess::deliver(registry, fd, identity);
  requireTestCondition(probe.calls == 1 && loop.timerCount() == 1);
  registry.beginConnectionsDrain();
  requireTestCondition(loop.timerCount() == 0);
  std::cout << "CASE E05 actual timer one-shot/reentry/close/fd reuse identity/drain PASS\n";
}
void testCapacity() {
  std::atomic<unsigned> executed{};
  auto gate = std::make_shared<Gate>();
  BoundedTaskExecutor executor;
  std::vector<std::shared_ptr<TaskJob>> jobs;
  for (unsigned i = 0; i < 2; ++i) {
    auto task = executor.submitTask(std::make_shared<Work>(executed, gate));
    requireTestCondition(task.admission == TaskAdmission::kAccepted);
    jobs.push_back(task.job);
    gate->wait(i + 1);
  }
  for (unsigned i = 0; i < 32; ++i) {
    auto task = executor.submitTask(std::make_shared<Work>(executed));
    requireTestCondition(task.admission == TaskAdmission::kAccepted);
    jobs.push_back(task.job);
  }
  auto full = executor.snapshot();
  requireTestCondition(full.running == 2 && full.queued == 32 && full.outstanding == 34);
  requireTestCondition(executor.submitTask(std::make_shared<Work>(executed)).admission ==
                       TaskAdmission::kOverloaded);
  gate->release();
  waitReady(executor, 34);
  requireTestCondition(executor.submitTask(std::make_shared<Work>(executed)).admission ==
                       TaskAdmission::kOverloaded);
  for (auto& job : jobs) requireTestCondition(executor.collectTask(job).work != nullptr);
  checkConservation(executor);
  requireTestCondition(executed == (std::getenv("HP_ASYNC_TASK_NEGATIVE") ? 35U : 34U));
  requireTestCondition(executor.snapshot().outstanding == 0);
  executor.stopAcceptingAndCancel();
  executor.joinWorkers();
  std::cout << "CASE E01 default 2/32/34 ready capacity PASS\n";
}
void testCancellation() {
  std::atomic<unsigned> executed{};
  auto gate = std::make_shared<Gate>();
  BoundedTaskExecutor executor({1, 2, 100ms});
  auto running = executor.submitTask(std::make_shared<Work>(executed, gate));
  gate->wait(1);
  auto queued = executor.submitTask(std::make_shared<Work>(executed));
  executor.cancelTask(queued.job);
  executor.cancelTask(running.job);
  requireTestCondition(executor.snapshot().outstanding == 1 && executor.snapshot().running == 1);
  executor.stopAcceptingAndCancel();
  requireTestCondition(executor.submitTask(std::make_shared<Work>(executed)).admission ==
                       TaskAdmission::kStopping);
  gate->release();
  executor.joinWorkers();
  checkConservation(executor);
  requireTestCondition(executed == 1 && executor.snapshot().outstanding == 0);
  std::cout << "CASE E02 running cancellation retains slot; queued cancellation skips work PASS\n";
}
void testExpiry(std::chrono::milliseconds timeout) {
  std::atomic<unsigned> executed{};
  auto gate = std::make_shared<Gate>();
  BoundedTaskExecutor executor({1, 2, timeout});
  auto first = executor.submitTask(std::make_shared<Work>(executed, gate));
  gate->wait(1);
  auto queued = executor.submitTask(std::make_shared<Work>(executed));
  std::this_thread::sleep_until(queued.job->deadline());
  gate->release();
  waitReady(executor, 2);
  requireTestCondition(executed == 1);
  executor.cancelTask(first.job);
  executor.cancelTask(queued.job);
  checkConservation(executor);
  requireTestCondition(executor.snapshot().outstanding == 0);
  std::cout << "CASE E03 deadline " << timeout.count() << "ms skips queued work PASS\n";
}
void testRaces() {
  std::atomic<unsigned> executed{};
  BoundedTaskExecutor executor;
  for (unsigned i = 0; i < 100; ++i) {
    auto job = executor.submitTask(std::make_shared<Work>(executed, nullptr, true)).job;
    requireTestCondition(job != nullptr);
    std::thread canceller([&executor, job] { executor.cancelTask(job); });
    (void)executor.collectTask(job);
    canceller.join();
    checkConservation(executor);
  }
  executor.stopAcceptingAndCancel();
  executor.joinWorkers();
  checkConservation(executor);
  requireTestCondition(executor.snapshot().outstanding == 0);
  std::cout << "CASE E04 exception/cancel/collect race PASS\n";
}
}  // namespace
int main() {
  testContinuationIdentity();
  testCapacity();
  testCancellation();
  testExpiry(100ms);
  testExpiry(1000ms);
  testRaces();
}
