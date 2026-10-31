#include "business/BoundedTaskExecutor.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace hp::business {
bool TaskCancellation::stopRequested() const noexcept {
  return cancelled.load(std::memory_order_acquire) || std::chrono::steady_clock::now() >= deadline;
}

BoundedTaskExecutor::BoundedTaskExecutor(TaskExecutorOptions options) : options_(options) {
  if (!options.threads || options.threads > 2 || !options.queueCapacity ||
      options.queueCapacity > 32 || options.timeout.count() <= 0)
    throw std::invalid_argument("invalid task executor limits");
  queue_.reserve(options.queueCapacity);
  workers_.reserve(options.threads);
  try {
    for (std::size_t i = 0; i < options.threads; ++i)
      workers_.emplace_back(&BoundedTaskExecutor::runWorkerTasks, this);
  } catch (...) {
    stopAcceptingAndCancel();
    joinWorkers();
    throw;
  }
}

BoundedTaskExecutor::~BoundedTaskExecutor() {
  stopAcceptingAndCancel();
  joinWorkers();
}

bool BoundedTaskExecutor::ownsJob(const std::shared_ptr<TaskJob>& job) const {
  return job && job->slot_ < slots_.size() && slots_[job->slot_] == job;
}

TaskSubmission BoundedTaskExecutor::submitTask(std::shared_ptr<TaskWork> work) {
  if (!work) throw std::invalid_argument("task work is required");
  auto job = std::make_shared<TaskJob>();
  std::lock_guard lock(mutex_);
  if (stopping_) return {TaskAdmission::kStopping, {}};
  if (queue_.size() == options_.queueCapacity ||
      counts_.outstanding == options_.threads + options_.queueCapacity)
    return {TaskAdmission::kOverloaded, {}};
  const auto slot = std::find(slots_.begin(), slots_.end(), nullptr);
  job->slot_ = static_cast<std::size_t>(slot - slots_.begin());
  job->deadline_ = std::chrono::steady_clock::now() + options_.timeout;
  job->work_ = std::move(work);
  queue_.push_back(job);
  *slot = job;
  ++counts_.queued;
  ++counts_.outstanding;
  ++counts_.admitted;
  available_.notify_one();
  return {TaskAdmission::kAccepted, std::move(job)};
}

std::shared_ptr<TaskWork> BoundedTaskExecutor::retireJob(const std::shared_ptr<TaskJob>& job) {
  if (job->state_ == TaskJobState::kQueued) --counts_.queued;
  if (job->state_ == TaskJobState::kRunning) --counts_.running;
  if (job->state_ == TaskJobState::kReady) --counts_.ready;
  job->state_ = TaskJobState::kRetired;
  --counts_.outstanding;
  ++counts_.released;
  slots_[job->slot_].reset();
  return std::move(job->work_);
}

std::shared_ptr<TaskWork> BoundedTaskExecutor::cancelLocked(const std::shared_ptr<TaskJob>& job) {
  if (!ownsJob(job)) return {};
  job->cancelled_.store(true, std::memory_order_release);
  if (job->state_ == TaskJobState::kRunning) return {};
  if (job->state_ == TaskJobState::kQueued)
    queue_.erase(std::find(queue_.begin(), queue_.end(), job));
  return retireJob(job);
}

void BoundedTaskExecutor::cancelTask(const std::shared_ptr<TaskJob>& job) noexcept {
  std::shared_ptr<TaskWork> discarded;
  {
    std::lock_guard lock(mutex_);
    discarded = cancelLocked(job);
  }
}

TaskCollection BoundedTaskExecutor::collectTask(const std::shared_ptr<TaskJob>& job) {
  std::lock_guard lock(mutex_);
  if (!ownsJob(job)) return {TaskJobState::kRetired, {}};
  if (job->state_ != TaskJobState::kReady) return {job->state_, {}};
  return {TaskJobState::kReady, retireJob(job)};
}

void BoundedTaskExecutor::stopAcceptingAndCancel() noexcept {
  std::array<std::shared_ptr<TaskWork>, 34> discarded;
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      auto job = slots_[i];
      if (job) discarded[i] = cancelLocked(job);
    }
    available_.notify_all();
  }
}

void BoundedTaskExecutor::joinWorkers() {
  for (auto& worker : workers_)
    if (worker.joinable()) worker.join();
}

TaskExecutorSnapshot BoundedTaskExecutor::snapshot() const {
  std::lock_guard lock(mutex_);
  return counts_;
}

void BoundedTaskExecutor::runWorkerTasks() {
  for (;;) {
    std::shared_ptr<TaskJob> job;
    std::shared_ptr<TaskWork> work;
    {
      std::unique_lock lock(mutex_);
      while (!stopping_ && queue_.empty()) available_.wait(lock);
      if (stopping_) return;
      job = queue_.front();
      queue_.erase(queue_.begin());
      --counts_.queued;
      ++counts_.running;
      job->state_ = TaskJobState::kRunning;
      work = job->work_;
    }
    const TaskCancellation cancellation{job->cancelled_, job->deadline_};
    try {
      if (!cancellation.stopRequested()) work->executeTask(cancellation);
    } catch (...) { /* 适配器负责异常响应；未产出结果由owner安全终结。 */
    }
    std::shared_ptr<TaskWork> discarded;
    {
      std::lock_guard lock(mutex_);
      if (job->cancelled_.load(std::memory_order_acquire))
        discarded = retireJob(job);
      else {
        --counts_.running;
        ++counts_.ready;
        job->state_ = TaskJobState::kReady;
      }
    }
  }
}
}  // namespace hp::business
