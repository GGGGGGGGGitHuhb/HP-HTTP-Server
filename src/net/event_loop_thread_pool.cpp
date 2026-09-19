#include "net/event_loop_thread_pool.h"

#include <stdexcept>
#include <utility>

#include "base/logger.h"

namespace hp::net {
struct EventLoopThreadPool::Ticket {
  EventLoopThreadPool& pool;
  std::size_t index;
  bool armed{false};

  ~Ticket() {
    if (armed) {
      std::lock_guard lock(pool.mutex_);
      --pool.outstanding_[index];
    }
  }
};

void EventLoopThreadPool::WorkerInitTask::InitializeWorker(
    EventLoop& loop) const {
  if (init) init(index, loop);
}

void EventLoopThreadPool::WorkerCleanupTask::CleanupWorker(
    EventLoop& loop) const {
  // A planned drain may finish one owner before its peers; fatal stops all.
  bool planned;
  {
    std::lock_guard lock(pool->mutex_);
    planned = pool->draining_;
  }
  if (!planned || loop.failed()) pool->RequestStop();
  if (cleanup) cleanup(index, loop);
}

void EventLoopThreadPool::ReservedPoolTask::RunTask(EventLoop& loop) const {
  task(loop);
}

EventLoopThreadPool::~EventLoopThreadPool() noexcept {
  try {
    RequestStop();
    Join();
  } catch (const std::exception& error) {
    base::Error(error.what());
  } catch (...) {
    base::Error("unobserved EventLoopThreadPool failure");
  }
}

void EventLoopThreadPool::Start(std::size_t count,
                                WorkerInitCallback init,
                                WorkerCleanupCallback cleanup) {
  {
    std::lock_guard lock(mutex_);
    if (started_) throw std::logic_error("pool already started");
    if (count > 64) throw std::invalid_argument("worker count exceeds 64");
    started_ = true;
    workers_.reserve(count);
    outstanding_.resize(count);
    for (std::size_t i = 0; i < count; ++i)
      workers_.push_back(std::make_unique<EventLoopThread>());
  }
  try {
    for (std::size_t i = 0; i < count; ++i) {
      workers_[i]->Start(std::bind_front(&WorkerInitTask::InitializeWorker,
                                         WorkerInitTask{init, i}),
                         std::bind_front(&WorkerCleanupTask::CleanupWorker,
                                         WorkerCleanupTask{this, cleanup, i}));
    }
    bool stop;
    {
      std::lock_guard lock(mutex_);
      ready_ = true;
      stop = stopping_;
    }
    if (stop) StopWorkers();
  } catch (...) {
    auto error = std::current_exception();
    RequestStop();
    try {
      Join();
    } catch (...) {
    }
    std::rethrow_exception(error);
  }
}

bool EventLoopThreadPool::Post(std::size_t index,
                               EventLoopThread::LoopTask task) {
  if (!task) throw std::invalid_argument("empty pool task");
  auto ticket = std::make_shared<Ticket>(Ticket{*this, index});
  auto* reservation = ticket.get();
  EventLoopThread::LoopTask queued =
      std::bind_front(&ReservedPoolTask::RunTask,
                      ReservedPoolTask{std::move(ticket), std::move(task)});
  {
    std::lock_guard lock(mutex_);
    if (!ready_ || stopping_ || index >= workers_.size() ||
        outstanding_[index] == kTaskCapacity)
      return false;
    ++outstanding_[index];
    reservation->armed = true;
    ++forwarding_;
  }
  // Stop records its cutoff now, but signals workers only after all accepted
  // forwards finish. No user capture is released under the pool mutex.
  try {
    const bool accepted = workers_[index]->Post(std::move(queued));
    FinishForward();
    return accepted;
  } catch (...) {
    FinishForward();
    throw;
  }
}

void EventLoopThreadPool::FinishForward() noexcept {
  bool stop;
  {
    std::lock_guard lock(mutex_);
    --forwarding_;
    stop = stopping_ && forwarding_ == 0;
    forwarded_.notify_all();
  }
  if (stop) StopWorkers();
}

void EventLoopThreadPool::StopWorkers() {
  for (auto& worker : workers_) worker->RequestStop();
}

void EventLoopThreadPool::RequestDrain(EventLoop::Deadline deadline) {
  {
    std::lock_guard lock(mutex_);
    draining_ = true;
  }
  for (auto& worker : workers_) worker->RequestDrain(deadline);
}

void EventLoopThreadPool::RequestForce() {
  {
    std::lock_guard lock(mutex_);
    draining_ = true;
  }
  for (auto& worker : workers_) worker->RequestForce();
}

void EventLoopThreadPool::RequestStop() {
  bool stop;
  {
    std::lock_guard lock(mutex_);
    if (!started_) return;
    stopping_ = true;
    stop = forwarding_ == 0;
  }
  if (stop) StopWorkers();
}

void EventLoopThreadPool::Join() {
  {
    std::unique_lock lock(mutex_);
    forwarded_.wait(lock, [this] { return forwarding_ == 0; });
  }
  std::exception_ptr error;
  for (auto& worker : workers_) {
    try {
      worker->Join();
    } catch (...) {
      if (!error) error = std::current_exception();
    }
  }
  if (error) std::rethrow_exception(error);
}
}  // namespace hp::net
