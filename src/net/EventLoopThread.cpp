#include "net/EventLoopThread.h"

#include <cassert>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "base/Logger.h"

namespace hp::net {
EventLoopThread::~EventLoopThread() noexcept {
  assert(std::this_thread::get_id() != workerId_);
  try {
    requestWorkerStop();
    joinWorkerThread();
  } catch (const std::exception& e) {
    hp::base::error(e.what());
  } catch (...) {
    hp::base::error("unobserved EventLoopThread failure");
  }
}

void EventLoopThread::LoopBoundTask::runTaskInWorkerLoop() const {
  // ReservedPoolTask::runTask()
  // *target 得到工作线程的 EventLoop 对象
  task(*workerEventLoop);
}

void EventLoopThread::createWorkerThread(InitCallback workerInitCallback,
                                         CleanupCallback workerCleanupCallback) {
  std::unique_lock lock(mutex_);
  if (started_) throw std::logic_error("EventLoopThread already started");
  started_ = true;
  try {
    // 创建工作线程并指定入口为 run()
    workerThread_ = std::thread(&EventLoopThread::runWorkerEventLoop,
                                this,
                                std::move(workerInitCallback),
                                std::move(workerCleanupCallback));
  } catch (...) {
    readyFlag_ = true;
    throw;
  }
  // wait() 条件为假则通过 lock 释放 mutex_ 并让主线程等待，以便让工作线程取得该锁
  // 主线程被唤醒后，wait() 重新取得锁，再检查条件
  initializationReady_.wait(lock, [this] { return isWorkerInitializationComplete(); });
  // 工作线程初始化失败
  if (!startupSucceeded_) {
    lock.unlock();
    joinWorkerThread();
  }
}

bool EventLoopThread::postTaskToWorker(TaskCallback task) {
  if (!task) throw std::invalid_argument("empty EventLoopThread task");
  // 先分配空任务槽及队列包装，再执行 noexcept 任务转移。
  // 两个所有权对象均在锁之前声明，确保用户捕获对象在解锁后释放。
  std::shared_ptr<LoopBoundTask> taskSlot;
  EventLoop::TaskCallback queuedTask;
  std::lock_guard lock(mutex_);
  if (!workerEventLoop_) return false;
  taskSlot = std::make_shared<LoopBoundTask>(LoopBoundTask{workerEventLoop_, {}});
  queuedTask = makeQueuedTask(taskSlot);
  // 把收到的任务存入 LoopBoundTask::task
  taskSlot->task = std::move(task);
  // Enqueue 可能立即唤醒所属线程：queued 必须是唯一所有者。
  taskSlot.reset();
  return workerEventLoop_->enqueueLoopTask(queuedTask);
}

EventLoop::TaskCallback EventLoopThread::makeQueuedTask(
    const std::shared_ptr<LoopBoundTask>& taskSlot) {
  return [taskSlot] { taskSlot->runTaskInWorkerLoop(); };
}

bool EventLoopThread::isWorkerInitializationComplete() const noexcept { return readyFlag_; }

void EventLoopThread::requestWorkerDrain(EventLoop::Deadline deadline) {
  std::lock_guard lock(mutex_);
  if (workerEventLoop_) workerEventLoop_->requestLoopDrain(deadline);
}

void EventLoopThread::requestWorkerForceClose() {
  std::lock_guard lock(mutex_);
  if (workerEventLoop_) workerEventLoop_->requestLoopForceClose();
}

void EventLoopThread::requestWorkerStop() {
  std::lock_guard lock(mutex_);
  if (!started_) return;
  stopRequested_ = true;
  if (workerEventLoop_) workerEventLoop_->requestLoopStop();
}

void EventLoopThread::joinWorkerThread() {
  {
    std::lock_guard lock(mutex_);
    if (std::this_thread::get_id() == workerId_)
      throw std::logic_error("EventLoopThread self join");
  }
  if (!workerThread_.joinable()) return;
  /*  主线程不能持锁等待，因为工作线程的 loop() 返回后，在 run() 中
   *  接下来要获取线程包装对象的锁，也就是拿到 EventLoopThread 实例的锁 mutex_
   *  如果主线程持锁等待，工作线程退出时等主线程放锁以撤销 loop_
   *  会出现死锁
   */
  // 等待工作线程退出
  workerThread_.join();
  std::exception_ptr error;
  {
    std::lock_guard lock(mutex_);
    // 取出异常
    error = std::exchange(failure_, {});
  }
  // 在主线程重新抛出异常
  if (error) std::rethrow_exception(error);
}

void EventLoopThread::runWorkerEventLoop(InitCallback workerInitCallback,
                                         CleanupCallback workerCleanupCallback) noexcept {
  std::exception_ptr error;
  {
    std::lock_guard lock(mutex_);
    workerId_ = std::this_thread::get_id();
  }
  try {
    // 工作线程创建自己的 loop
    EventLoop workerEventLoop;
    try {
      // 绑定 `TcpServer::initializeWorkerRegistry()`
      if (workerInitCallback) workerInitCallback(workerEventLoop);
      {
        std::lock_guard lock(mutex_);
        if (stopRequested_) workerEventLoop.requestLoopStop();
        workerEventLoop_ = &workerEventLoop;
        startupSucceeded_ = true;
        readyFlag_ = true;
      }
      initializationReady_.notify_all();  // 通知等待方重新检查条件
      workerEventLoop.runEventLoop();     // 工作线程开始自己的事件循环
    } catch (...) {
      error = std::current_exception();
    }
    {
      // 取得锁并撤销 loop_
      std::lock_guard lock(mutex_);
      workerEventLoop_ = nullptr;
    }
    try {
      // 绑定 `EventLoopThreadPool::WorkerCleanupTask::cleanupWorker()`
      if (workerCleanupCallback) workerCleanupCallback(workerEventLoop);
    } catch (...) {
      if (!error) error = std::current_exception();
    }
  } catch (...) {
    if (!error) error = std::current_exception();
  }
  {
    std::lock_guard lock(mutex_);
    // 保存异常
    failure_ = error;
    readyFlag_ = true;
  }
  initializationReady_.notify_all();
}
}  // namespace hp::net
