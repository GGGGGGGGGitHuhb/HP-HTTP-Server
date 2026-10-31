#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "TestCheck.h"
#include "business/BoundedTaskExecutor.h"
#include "business/TaskService.h"
#include "http/TaskApiHandler.h"

namespace {
using namespace hp::business;
using namespace std::chrono_literals;
constexpr unsigned kProducers = 8;
constexpr unsigned kOperations = 32;

class StoreProbe {
 public:
  void createRecords(unsigned producer);
  void deleteRecords(unsigned producer);
  void replaceAndObserve(unsigned producer);
  void verifyCapacityResponse() const;

  std::shared_ptr<TaskService> service{std::make_shared<TaskService>()};
  std::barrier<> phase{kProducers};
  std::array<std::vector<TaskId>, kProducers> identifiers;
  TaskId sharedId{};
};

bool validPair(const TaskInput& input) {
  return input.title == (input.completed ? "complete" : "pending");
}

void StoreProbe::createRecords(unsigned producer) {
  phase.arrive_and_wait();
  for (unsigned operation = 0; operation < kOperations; ++operation) {
    const auto result = service->createTask({std::string(256, 'x'), false});
    requireTestCondition(result.status == TaskStatus::kSuccess ||
                         result.status == TaskStatus::kCapacity);
    if (result.task) identifiers[producer].push_back(result.task->id);
    requireTestCondition(service->listTasks().size() <= kMaxTasks);
  }
}

void StoreProbe::deleteRecords(unsigned producer) {
  phase.arrive_and_wait();
  for (const auto id : identifiers[producer])
    requireTestCondition(service->deleteTask(id).status == TaskStatus::kSuccess);
}

void StoreProbe::replaceAndObserve(unsigned producer) {
  phase.arrive_and_wait();
  for (unsigned operation = 0; operation < kOperations; ++operation) {
    const bool completed = producer % 2 != 0;
    requireTestCondition(
        service->replaceTask(sharedId, {completed ? "complete" : "pending", completed}).status ==
        TaskStatus::kSuccess);
    const auto observed = service->getTask(sharedId);
    const auto snapshot = service->listTasks();
    requireTestCondition(observed.task && validPair(observed.task->input));
    requireTestCondition(snapshot.size() == 1 && snapshot.front().id == sharedId &&
                         validPair(snapshot.front().input));
  }
}

void StoreProbe::verifyCapacityResponse() const {
  hp::http::TaskApiHandler handler(service);
  hp::http::HttpRequest request;
  request.method = "GET";
  const auto response = handler.onTaskResponse({hp::http::TaskRouteKind::kCollection, 0},
                                               request,
                                               hp::http::ConnectionPolicy::kKeepAlive);
  requireTestCondition(response.status == hp::http::Status::kOk);
  requireTestCondition(response.contentBytes <= hp::http::kMaxTaskResponseBytes);
}

void runStorePhase(StoreProbe& probe, void (StoreProbe::*operation)(unsigned)) {
  std::vector<std::jthread> producers;
  for (unsigned producer = 0; producer < kProducers; ++producer)
    producers.emplace_back(operation, &probe, producer);
  for (auto& producer : producers) producer.join();
}

void testStoreComposition() {
  StoreProbe probe;
  runStorePhase(probe, &StoreProbe::createRecords);
  std::set<TaskId> unique;
  std::size_t count = 0;
  for (const auto& ids : probe.identifiers) {
    count += ids.size();
    unique.insert(ids.begin(), ids.end());
  }
  requireTestCondition(count == kMaxTasks && unique.size() == count);
  requireTestCondition(probe.service->listTasks().size() == count);
  probe.verifyCapacityResponse();
  runStorePhase(probe, &StoreProbe::deleteRecords);
  requireTestCondition(probe.service->listTasks().empty());
  probe.sharedId = probe.service->createTask({"pending", false}).task->id;
  requireTestCondition(probe.sharedId > *unique.rbegin());
  runStorePhase(probe, &StoreProbe::replaceAndObserve);
  requireTestCondition(validPair(probe.service->getTask(probe.sharedId).task->input));
  requireTestCondition(std::getenv("HP_APPLICATION_NEGATIVE") == nullptr);
  std::cout << "CASE S301 store 8x32 capacity/unique/delete/whole-object/list PASS\n";
}

class WorkGate {
 public:
  void enter();
  void waitEntered();
  void release();
  bool released() const { return released_; }
  bool bothEntered() const { return entered_ == 2; }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool released_{};
  unsigned entered_{};
};

void WorkGate::enter() {
  std::unique_lock lock(mutex_);
  ++entered_;
  changed_.notify_all();
  requireTestCondition(changed_.wait_for(lock, 5s, [this] { return released(); }));
}

void WorkGate::waitEntered() {
  std::unique_lock lock(mutex_);
  requireTestCondition(changed_.wait_for(lock, 5s, [this] { return bothEntered(); }));
}

void WorkGate::release() {
  std::lock_guard lock(mutex_);
  released_ = true;
  changed_.notify_all();
}

class SnapshotWork final : public TaskWork {
 public:
  SnapshotWork(std::shared_ptr<TaskService> service, std::shared_ptr<WorkGate> gate = {});
  void executeTask(const TaskCancellation& cancellation) override;
  const std::vector<Task>& records() const { return records_; }

 private:
  std::shared_ptr<TaskService> service_;
  std::shared_ptr<WorkGate> gate_;
  std::vector<Task> records_;
};

SnapshotWork::SnapshotWork(std::shared_ptr<TaskService> service, std::shared_ptr<WorkGate> gate)
    : service_(std::move(service)), gate_(std::move(gate)) {}

void SnapshotWork::executeTask(const TaskCancellation&) {
  if (gate_) gate_->enter();
  records_ = service_->listTasks();
}

void checkSnapshot(const BoundedTaskExecutor& executor) {
  const auto value = executor.snapshot();
  requireTestCondition(value.queued <= 32 && value.running <= 2 && value.outstanding <= 34);
  requireTestCondition(value.outstanding == value.queued + value.running + value.ready);
  requireTestCondition(value.admitted == value.released + value.outstanding);
}

void waitReady(const BoundedTaskExecutor& executor, std::size_t count) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (executor.snapshot().ready != count) {
    checkSnapshot(executor);
    requireTestCondition(std::chrono::steady_clock::now() < deadline);
    std::this_thread::yield();
  }
}

class ExecutorProbe {
 public:
  void raceJobs(unsigned producer);
  BoundedTaskExecutor executor{{2, 32, 5000ms}};
  std::barrier<> phase{kProducers};
  std::array<std::shared_ptr<TaskJob>, 34> jobs;
  std::atomic<unsigned> collected{};
};

void ExecutorProbe::raceJobs(unsigned producer) {
  phase.arrive_and_wait();
  for (unsigned operation = 0; operation < kOperations; ++operation) {
    const auto& job = jobs[(producer + operation) % jobs.size()];
    if (producer % 2 == 0) {
      executor.cancelTask(job);
    } else {
      auto result = executor.collectTask(job);
      if (result.work) {
        requireTestCondition(result.state == TaskJobState::kReady);
        const auto work = std::dynamic_pointer_cast<SnapshotWork>(result.work);
        requireTestCondition(work && work->records().size() == 1 &&
                             work->records().front().input.title == "stable");
        ++collected;
      }
    }
    checkSnapshot(executor);
  }
}

void testExecutorComposition() {
  auto service = std::make_shared<TaskService>();
  requireTestCondition(service->createTask({"stable", false}).status == TaskStatus::kSuccess);
  auto gate = std::make_shared<WorkGate>();
  ExecutorProbe probe;
  for (unsigned i = 0; i < 2; ++i) {
    auto submission = probe.executor.submitTask(std::make_shared<SnapshotWork>(service, gate));
    requireTestCondition(submission.admission == TaskAdmission::kAccepted);
    probe.jobs[i] = submission.job;
  }
  gate->waitEntered();
  for (unsigned i = 2; i < probe.jobs.size(); ++i) {
    auto submission = probe.executor.submitTask(std::make_shared<SnapshotWork>(service));
    requireTestCondition(submission.admission == TaskAdmission::kAccepted);
    probe.jobs[i] = submission.job;
  }
  checkSnapshot(probe.executor);
  requireTestCondition(probe.executor.snapshot().queued == 32);
  requireTestCondition(
      probe.executor.submitTask(std::make_shared<SnapshotWork>(service)).admission ==
      TaskAdmission::kOverloaded);
  // 排队取消立即退槽；运行取消只能在任务实际返回后退槽。
  probe.executor.cancelTask(probe.jobs[2]);
  probe.executor.cancelTask(probe.jobs[0]);
  requireTestCondition(probe.executor.snapshot().outstanding == 33);
  gate->release();
  waitReady(probe.executor, 32);
  requireTestCondition(probe.executor.snapshot().outstanding == 32);
  for (unsigned i : {0U, 2U}) {
    auto submission = probe.executor.submitTask(std::make_shared<SnapshotWork>(service));
    requireTestCondition(submission.admission == TaskAdmission::kAccepted);
    probe.jobs[i] = submission.job;
  }
  waitReady(probe.executor, 34);
  requireTestCondition(
      probe.executor.submitTask(std::make_shared<SnapshotWork>(service)).admission ==
      TaskAdmission::kOverloaded);
  std::vector<std::jthread> producers;
  for (unsigned producer = 0; producer < kProducers; ++producer)
    producers.emplace_back(&ExecutorProbe::raceJobs, &probe, producer);
  for (auto& producer : producers) producer.join();
  probe.executor.stopAcceptingAndCancel();
  probe.executor.joinWorkers();
  checkSnapshot(probe.executor);
  requireTestCondition(probe.executor.snapshot().outstanding == 0 && probe.collected <= 34);
  std::cout << "CASE S302 executor ready-budget/cancel/collect 8x32 conservation PASS\n";
}
}  // namespace

int main() {
  testStoreComposition();
  testExecutorComposition();
}
