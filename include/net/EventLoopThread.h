#pragma once
#include <condition_variable>

#include "net/EventLoop.h"

namespace hp::net {
// Control thread serializes start/join/destruction. Concurrent post/stop
// callers must finish before destruction. No loop pointer escapes this wrapper.
class EventLoopThread final : private base::NonCopyable {
 public:
  using InitCallback = std::function<void(EventLoop&)>;

  using CleanupCallback = std::function<void(EventLoop&)>;
  using TaskCallback = std::function<void(EventLoop&)>;

  EventLoopThread() = default;
  ~EventLoopThread() noexcept;

  void start(InitCallback init = {}, CleanupCallback cleanup = {});

  bool post(TaskCallback task);

  void requestStop();
  void requestDrain(EventLoop::Deadline deadline);
  void requestForce();

  void join();

 private:
  struct LoopBoundTask {
    EventLoop* target;
    TaskCallback task;
    void runInLoop() const;
  };

  static EventLoop::TaskCallback makeQueuedTask(
      const std::shared_ptr<LoopBoundTask>& slot);
  bool isReady() const noexcept;

  void run(InitCallback init, CleanupCallback cleanup) noexcept;

  std::mutex mutex_;
  std::condition_variable ready_;

  std::thread thread_;
  std::thread::id workerId_{};

  EventLoop* loop_{nullptr};
  bool started_{false}, readyFlag_{false}, startupSucceeded_{false},
      stop_{false};
  std::exception_ptr failure_;
};
}  // namespace hp::net
