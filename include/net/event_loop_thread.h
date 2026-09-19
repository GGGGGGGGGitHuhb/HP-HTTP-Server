#pragma once
#include <condition_variable>

#include "net/event_loop.h"

namespace hp::net {
// Control thread serializes start/join/destruction. Concurrent post/stop
// callers must finish before destruction. No loop pointer escapes this wrapper.
class EventLoopThread final : private base::NonCopyable {
 public:
  using InitCallback = std::function<void(EventLoop&)>;

  using CleanupCallback = std::function<void(EventLoop&)>;
  using LoopTask = std::function<void(EventLoop&)>;

  EventLoopThread() = default;
  ~EventLoopThread() noexcept;

  void Start(InitCallback init = {}, CleanupCallback cleanup = {});

  bool Post(LoopTask task);

  void RequestStop();
  void RequestDrain(EventLoop::Deadline deadline);
  void RequestForce();

  void Join();

 private:
  struct LoopBoundTask {
    EventLoop* target;
    LoopTask task;
    void RunInLoop() const;
  };

  void Run(InitCallback init, CleanupCallback cleanup) noexcept;

  std::mutex mutex_;
  std::condition_variable ready_;

  std::thread thread_;
  std::thread::id worker_id_{};

  EventLoop* loop_{nullptr};
  bool started_{false}, ready_flag_{false}, startup_succeeded_{false},
      stop_{false};
  std::exception_ptr failure_;
};
}  // namespace hp::net
