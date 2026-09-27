#include "net/EventLoopThreadPool.h"

#include <stdexcept>
#include <utility>

#include "base/Logger.h"

namespace hp::net {
struct EventLoopThreadPool::Ticket {
  EventLoopThreadPool& workerPool;
  std::size_t workerIndex;
  bool armed{false};

  ~Ticket() {
    if (armed) {
      std::lock_guard lock(workerPool.mutex_);
      --workerPool.outstanding_[workerIndex];
    }
  }
};

void EventLoopThreadPool::WorkerInitTask::initializeWorker(EventLoop& workerEventLoop) const {
  if (workerInitCallback) workerInitCallback(workerIndex, workerEventLoop);
}

void EventLoopThreadPool::WorkerCleanupTask::cleanupWorker(EventLoop& workerEventLoop) const {
  // 计划内的排空允许某个所属线程先于其他线程结束；致命错误则停止全部线程。
  bool planned;
  {
    std::lock_guard lock(workerPool->mutex_);
    planned = workerPool->draining_;
  }
  if (!planned || workerEventLoop.failed()) workerPool->requestPoolStop();
  if (workerCleanupCallback) workerCleanupCallback(workerIndex, workerEventLoop);
}

void EventLoopThreadPool::ReservedPoolTask::runTask(EventLoop& workerEventLoop) const {
  task(workerEventLoop);
}

EventLoopThreadPool::~EventLoopThreadPool() noexcept {
  try {
    requestPoolStop();
    joinWorkerThreads();
  } catch (const std::exception& error) {
    base::error(error.what());
  } catch (...) {
    base::error("unobserved EventLoopThreadPool failure");
  }
}

void EventLoopThreadPool::createWorkerThreads(std::size_t workerCount,
                                              WorkerInitCallback workerInitCallback,
                                              WorkerCleanupCallback workerCleanupCallback) {
  {
    std::lock_guard lock(mutex_);
    if (started_) throw std::logic_error("pool already started");
    if (workerCount > 64) throw std::invalid_argument("worker count exceeds 64");
    started_ = true;
    workers_.reserve(workerCount);
    outstanding_.resize(workerCount);
    for (std::size_t i = 0; i < workerCount; ++i)
      workers_.push_back(std::make_unique<EventLoopThread>());
  }
  try {
    for (std::size_t i = 0; i < workerCount; ++i) {
      workers_[i]->createWorkerThread(
          [target = WorkerInitTask{workerInitCallback, i}](EventLoop& workerEventLoop) {
            target.initializeWorker(workerEventLoop);
          },
          [target = WorkerCleanupTask{this, workerCleanupCallback, i}](EventLoop& workerEventLoop) {
            target.cleanupWorker(workerEventLoop);
          });
    }
    bool shouldRequestWorkerStop;
    {
      std::lock_guard lock(mutex_);
      ready_ = true;
      shouldRequestWorkerStop = stopping_;
    }
    if (shouldRequestWorkerStop) requestAllWorkersStop();
  } catch (...) {
    auto error = std::current_exception();
    requestPoolStop();
    try {
      joinWorkerThreads();
    } catch (...) {
    }
    std::rethrow_exception(error);
  }
}

bool EventLoopThreadPool::postTaskToWorkerAtIndex(std::size_t workerIndex,
                                                  EventLoopThread::TaskCallback task) {
  if (!task) throw std::invalid_argument("empty pool task");
  auto ticket = std::make_shared<Ticket>(Ticket{*this, workerIndex});
  auto* reservation = ticket.get();
  // 把 task 也就是 TcpServer::adoptConnection() 包装为新 lambda
  // 交给 queue，之后交给 EventLoopThread::post()
  EventLoopThread::TaskCallback queuedTask =
      [target = ReservedPoolTask{std::move(ticket), std::move(task)}](EventLoop& workerEventLoop) {
        // 保存原始 lambda，TcpServer::adoptConnection()
        target.runTask(workerEventLoop);
      };
  {
    std::lock_guard lock(mutex_);
    if (!ready_ || stopping_ || workerIndex >= workers_.size() ||
        outstanding_[workerIndex] == kTaskCapacity)
      return false;
    ++outstanding_[workerIndex];
    reservation->armed = true;
    ++forwarding_;
  }
  // Stop 此时记录停止接收的界限，但必须等所有已接受的
  // 转发完成才通知工作线程。不得在持有线程池互斥锁时释放用户捕获对象。
  try {
    // 用 index 让任务进入选中的 worker
    const bool accepted = workers_[workerIndex]->postTaskToWorker(std::move(queuedTask));
    finishTaskForward();
    return accepted;
  } catch (...) {
    finishTaskForward();
    throw;
  }
}

void EventLoopThreadPool::finishTaskForward() noexcept {
  bool shouldRequestWorkerStop;
  {
    std::lock_guard lock(mutex_);
    --forwarding_;
    shouldRequestWorkerStop = stopping_ && forwarding_ == 0;
    forwarded_.notify_all();
  }
  if (shouldRequestWorkerStop) requestAllWorkersStop();
}

void EventLoopThreadPool::requestAllWorkersStop() {
  for (auto& worker : workers_) worker->requestWorkerStop();
}

void EventLoopThreadPool::requestWorkersDrain(EventLoop::Deadline deadline) {
  {
    std::lock_guard lock(mutex_);
    draining_ = true;
  }
  for (auto& worker : workers_) worker->requestWorkerDrain(deadline);
}

void EventLoopThreadPool::requestWorkersForceClose() {
  {
    std::lock_guard lock(mutex_);
    draining_ = true;
  }
  for (auto& worker : workers_) worker->requestWorkerForceClose();
}

void EventLoopThreadPool::requestPoolStop() {
  bool shouldRequestWorkerStop;
  {
    std::lock_guard lock(mutex_);
    if (!started_) return;
    stopping_ = true;
    shouldRequestWorkerStop = forwarding_ == 0;
  }
  if (shouldRequestWorkerStop) requestAllWorkersStop();
}

bool EventLoopThreadPool::taskForwardsFinished() const noexcept { return forwarding_ == 0; }

void EventLoopThreadPool::joinWorkerThreads() {
  {
    std::unique_lock lock(mutex_);
    forwarded_.wait(lock, [this] { return taskForwardsFinished(); });
  }
  std::exception_ptr error;
  for (auto& worker : workers_) {
    try {
      worker->joinWorkerThread();
    } catch (...) {
      if (!error) error = std::current_exception();
    }
  }
  if (error) std::rethrow_exception(error);
}
}  // namespace hp::net
