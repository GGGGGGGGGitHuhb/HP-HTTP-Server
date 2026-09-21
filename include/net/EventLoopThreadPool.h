#pragma once
#include <vector>

#include "net/EventLoopThread.h"

namespace hp::net {
struct EventLoopThreadPoolTestAccess;

// Fixed workers; start/join/destruction are serialized by the control thread.
// Concurrent post/stop callers must finish before destruction.
class EventLoopThreadPool final : private base::NonCopyable {
 public:
  using WorkerInitCallback = std::function<void(std::size_t, EventLoop&)>;

  using WorkerCleanupCallback = std::function<void(std::size_t, EventLoop&)>;

  static constexpr std::size_t kTaskCapacity = 1024;

  EventLoopThreadPool() = default;
  ~EventLoopThreadPool() noexcept;

  void start(std::size_t count,
             WorkerInitCallback init = {},
             WorkerCleanupCallback cleanup = {});

  bool post(std::size_t index, EventLoopThread::TaskCallback task);

  void requestStop();
  void requestDrain(EventLoop::Deadline deadline);
  void requestForce();

  void join();

 private:
  friend struct EventLoopThreadPoolTestAccess;

  struct Ticket;

  struct WorkerInitTask {
    WorkerInitCallback init;
    std::size_t index;
    void initializeWorker(EventLoop& loop) const;
  };

  struct WorkerCleanupTask {
    EventLoopThreadPool* pool;
    WorkerCleanupCallback cleanup;
    std::size_t index;
    void cleanupWorker(EventLoop& loop) const;
  };

  struct ReservedPoolTask {
    // Declaration order matters: user captures die before quota is returned.
    std::shared_ptr<Ticket> ticket;
    EventLoopThread::TaskCallback task;
    void runTask(EventLoop& loop) const;
  };

  bool forwardsFinished() const noexcept;
  void finishForward() noexcept;
  void stopWorkers();

  std::mutex mutex_;
  std::condition_variable forwarded_;

  std::vector<std::unique_ptr<EventLoopThread>> workers_;
  std::vector<std::size_t> outstanding_;
  std::size_t forwarding_{0};

  bool draining_{false};
  bool started_{false}, ready_{false}, stopping_{false};
};
}  // namespace hp::net
