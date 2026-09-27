#pragma once
#include <vector>

#include "net/EventLoopThread.h"

namespace hp::net {
struct EventLoopThreadPoolTestAccess;

// 固定工作线程；控制线程串行执行 start/join/析构。
// 并发 post/stop 调用方必须在析构前完成调用。
class EventLoopThreadPool final : private base::NonCopyable {
 public:
  using WorkerInitCallback = std::function<void(std::size_t, EventLoop&)>;

  using WorkerCleanupCallback = std::function<void(std::size_t, EventLoop&)>;

  static constexpr std::size_t kTaskCapacity = 1024;

  EventLoopThreadPool() = default;
  ~EventLoopThreadPool() noexcept;

  void createWorkerThreads(std::size_t workerCount,
                           WorkerInitCallback workerInitCallback = {},
                           WorkerCleanupCallback workerCleanupCallback = {});

  bool postTaskToWorkerAtIndex(std::size_t workerIndex, EventLoopThread::TaskCallback task);

  void requestPoolStop();
  void requestWorkersDrain(EventLoop::Deadline deadline);
  void requestWorkersForceClose();

  void joinWorkerThreads();

 private:
  friend struct EventLoopThreadPoolTestAccess;

  struct Ticket;

  struct WorkerInitTask {
    WorkerInitCallback workerInitCallback;
    std::size_t workerIndex;
    void initializeWorker(EventLoop& workerEventLoop) const;
  };

  struct WorkerCleanupTask {
    EventLoopThreadPool* workerPool;
    WorkerCleanupCallback workerCleanupCallback;
    std::size_t workerIndex;
    void cleanupWorker(EventLoop& workerEventLoop) const;
  };

  struct ReservedPoolTask {
    // 声明顺序很重要：用户捕获对象先析构，随后才归还配额。
    std::shared_ptr<Ticket> ticket;
    EventLoopThread::TaskCallback task;
    void runTask(EventLoop& workerEventLoop) const;
  };

  bool taskForwardsFinished() const noexcept;
  void finishTaskForward() noexcept;
  void requestAllWorkersStop();

  std::mutex mutex_;
  std::condition_variable forwarded_;

  std::vector<std::unique_ptr<EventLoopThread>> workers_;
  std::vector<std::size_t> outstanding_;
  std::size_t forwarding_{0};

  bool draining_{false};
  bool started_{false}, ready_{false}, stopping_{false};
};
}  // namespace hp::net
