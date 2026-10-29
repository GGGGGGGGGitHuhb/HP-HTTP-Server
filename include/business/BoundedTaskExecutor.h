#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace hp::business {
struct TaskCancellation {
  const std::atomic<bool>& cancelled;
  std::chrono::steady_clock::time_point deadline;
  [[nodiscard]] bool stopRequested() const noexcept;
};

class TaskWork {
 public:
  virtual ~TaskWork() = default;
  virtual void executeTask(const TaskCancellation& cancellation) = 0;
};

enum class TaskAdmission { kAccepted, kOverloaded, kStopping };
enum class TaskJobState { kQueued, kRunning, kReady, kRetired };

struct TaskExecutorOptions {
  std::size_t threads{2}, queueCapacity{32};
  std::chrono::milliseconds timeout{1000};
};

struct TaskExecutorSnapshot {
  std::size_t queued{0}, running{0}, ready{0}, outstanding{0};
  std::uint64_t admitted{0}, released{0};
};

class TaskJob final {
 public:
  [[nodiscard]] std::chrono::steady_clock::time_point deadline() const noexcept {
    return deadline_;
  }

 private:
  friend class BoundedTaskExecutor;

  TaskJobState state_{TaskJobState::kQueued};
  std::atomic<bool> cancelled_{false};
  std::chrono::steady_clock::time_point deadline_;
  std::size_t slot_{0};
  std::shared_ptr<TaskWork> work_;
};

struct TaskSubmission {
  TaskAdmission admission;
  std::shared_ptr<TaskJob> job{};
};

struct TaskCollection {
  TaskJobState state;
  std::shared_ptr<TaskWork> work{};
};

class BoundedTaskExecutor final {
 public:
  explicit BoundedTaskExecutor(TaskExecutorOptions options = {});
  ~BoundedTaskExecutor();
  BoundedTaskExecutor(const BoundedTaskExecutor&) = delete;
  BoundedTaskExecutor& operator=(const BoundedTaskExecutor&) = delete;

  [[nodiscard]] TaskSubmission submitTask(std::shared_ptr<TaskWork> work);
  [[nodiscard]] TaskCollection collectTask(const std::shared_ptr<TaskJob>& job);
  void cancelTask(const std::shared_ptr<TaskJob>& job) noexcept;
  void stopAcceptingAndCancel() noexcept;
  void joinWorkers();
  [[nodiscard]] TaskExecutorSnapshot snapshot() const;

 private:
  void runWorkerTasks();
  bool ownsJob(const std::shared_ptr<TaskJob>& job) const;
  std::shared_ptr<TaskWork> retireJob(const std::shared_ptr<TaskJob>& job);
  std::shared_ptr<TaskWork> cancelLocked(const std::shared_ptr<TaskJob>& job);

  const TaskExecutorOptions options_;
  mutable std::mutex mutex_;
  std::condition_variable available_;
  bool stopping_{false};
  std::vector<std::shared_ptr<TaskJob>> queue_;
  std::array<std::shared_ptr<TaskJob>, 34> slots_{};
  std::vector<std::thread> workers_;
  TaskExecutorSnapshot counts_;
};
}  // namespace hp::business
