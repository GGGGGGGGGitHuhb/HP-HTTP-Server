#pragma once
#include <condition_variable>

#include "net/event_loop.h"

namespace hp::net {
// Control thread serializes start/join/destruction. Concurrent post/stop
// callers must finish before destruction. No loop pointer escapes this wrapper.
class EventLoopThread final : private base::NonCopyable {
 public:
  using Callback = std::function<void(EventLoop&)>;

  EventLoopThread() = default;
  ~EventLoopThread() noexcept;

  void start(Callback init = {}, Callback cleanup = {});

  bool post(Callback task);

  void request_stop();
  void request_drain(EventLoop::Deadline deadline);
  void request_force();

  void join();

 private:
  void run(Callback init, Callback cleanup) noexcept;

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
