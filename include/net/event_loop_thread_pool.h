#pragma once
#include <vector>

#include "net/event_loop_thread.h"

namespace hp::net {
struct EventLoopThreadPoolTestAccess;

// Fixed workers; start/join/destruction are serialized by the control thread.
// Concurrent post/stop callers must finish before destruction.
class EventLoopThreadPool final : private base::NonCopyable {
 public:
  using Callback = std::function<void(std::size_t, EventLoop&)>;

  static constexpr std::size_t task_capacity = 1024;

  EventLoopThreadPool() = default;
  ~EventLoopThreadPool() noexcept;

  void start(std::size_t count, Callback init = {}, Callback cleanup = {});

  bool post(std::size_t index, EventLoopThread::Callback task);

  void request_stop();
  void request_drain(EventLoop::Deadline deadline);
  void request_force();

  void join();

 private:
  friend struct EventLoopThreadPoolTestAccess;

  struct Ticket;

  void finish_forward() noexcept;
  void stop_workers();

  std::mutex mutex_;
  std::condition_variable forwarded_;

  std::vector<std::unique_ptr<EventLoopThread>> workers_;
  std::vector<std::size_t> outstanding_;
  std::size_t forwarding_{0};

  bool draining_{false};
  bool started_{false}, ready_{false}, stopping_{false};
};
}  // namespace hp::net
