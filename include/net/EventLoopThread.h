#pragma once
#include <condition_variable>

#include "net/EventLoop.h"

namespace hp::net {
// 控制线程串行执行 start/join/析构。并发 post/stop
// 调用方必须在析构前完成调用。事件循环指针不得逃逸出此包装对象。
class EventLoopThread final : private base::NonCopyable {
 public:
  using InitCallback = std::function<void(EventLoop&)>;

  using CleanupCallback = std::function<void(EventLoop&)>;
  using TaskCallback = std::function<void(EventLoop&)>;

  EventLoopThread() = default;
  ~EventLoopThread() noexcept;

  // 主线程运行入口
  void createWorkerThread(InitCallback workerInitCallback = {},
                          CleanupCallback workerCleanupCallback = {});

  bool postTaskToWorker(TaskCallback task);

  void requestWorkerStop();
  void requestWorkerDrain(EventLoop::Deadline deadline);
  void requestWorkerForceClose();

  void joinWorkerThread();

 private:
  struct LoopBoundTask {
    EventLoop* workerEventLoop;
    TaskCallback task;
    void runTaskInWorkerLoop() const;
  };

  static EventLoop::TaskCallback makeQueuedTask(const std::shared_ptr<LoopBoundTask>& taskSlot);
  bool isWorkerInitializationComplete() const noexcept;

  // 工作线程运行入口
  void runWorkerEventLoop(InitCallback workerInitCallback,
                          CleanupCallback workerCleanupCallback) noexcept;

  std::mutex mutex_;
  std::condition_variable initializationReady_;

  std::thread workerThread_;
  std::thread::id workerId_{};

  EventLoop* workerEventLoop_{nullptr};
  bool started_{false}, readyFlag_{false},  // 启动是否有结果
      startupSucceeded_{false},             // 初始化是否成功
      stopRequested_{false};
  std::exception_ptr failure_;
};
}  // namespace hp::net
