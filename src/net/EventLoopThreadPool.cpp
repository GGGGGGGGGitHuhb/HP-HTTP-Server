#include "net/EventLoopThreadPool.h"

#include <stdexcept>
#include <utility>

#include "base/Logger.h"

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

void EventLoopThreadPool::WorkerInitTask::initializeWorker(
    EventLoop& loop) const {
  if (init) init(index, loop);
}

void EventLoopThreadPool::WorkerCleanupTask::cleanupWorker(
    EventLoop& loop) const {
  // A planned drain may finish one owner before its peers; fatal stops all.
  bool planned;
  {
    std::lock_guard lock(pool->mutex_);
    planned = pool->draining_;
  }
  if (!planned || loop.failed()) pool->requestStop();
  if (cleanup) cleanup(index, loop);
}

void EventLoopThreadPool::ReservedPoolTask::runTask(EventLoop& loop) const {
  task(loop);
}

EventLoopThreadPool::~EventLoopThreadPool() noexcept {
  try {
    requestStop();
    join();
  } catch (const std::exception& error) {
    base::error(error.what());
  } catch (...) {
    base::error("unobserved EventLoopThreadPool failure");
  }
}

void EventLoopThreadPool::start(std::size_t count,
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
      workers_[i]->start(
          [target = WorkerInitTask{init, i}](EventLoop& loop) {
            target.initializeWorker(loop);
          },
          [target = WorkerCleanupTask{this, cleanup, i}](EventLoop& loop) {
            target.cleanupWorker(loop);
          });
    }
    bool stop;
    {
      std::lock_guard lock(mutex_);
      ready_ = true;
      stop = stopping_;
    }
    if (stop) stopWorkers();
  } catch (...) {
    auto error = std::current_exception();
    requestStop();
    try {
      join();
    } catch (...) {
    }
    std::rethrow_exception(error);
  }
}

bool EventLoopThreadPool::post(std::size_t index,
                               EventLoopThread::TaskCallback task) {
  if (!task) throw std::invalid_argument("empty pool task");
  auto ticket = std::make_shared<Ticket>(Ticket{*this, index});
  auto* reservation = ticket.get();
  EventLoopThread::TaskCallback queued =
      [target = ReservedPoolTask{std::move(ticket), std::move(task)}](
          EventLoop& loop) { target.runTask(loop); };
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
    const bool accepted = workers_[index]->post(std::move(queued));
    finishForward();
    return accepted;
  } catch (...) {
    finishForward();
    throw;
  }
}

void EventLoopThreadPool::finishForward() noexcept {
  bool stop;
  {
    std::lock_guard lock(mutex_);
    --forwarding_;
    stop = stopping_ && forwarding_ == 0;
    forwarded_.notify_all();
  }
  if (stop) stopWorkers();
}

void EventLoopThreadPool::stopWorkers() {
  for (auto& worker : workers_) worker->requestStop();
}

void EventLoopThreadPool::requestDrain(EventLoop::Deadline deadline) {
  {
    std::lock_guard lock(mutex_);
    draining_ = true;
  }
  for (auto& worker : workers_) worker->requestDrain(deadline);
}

void EventLoopThreadPool::requestForce() {
  {
    std::lock_guard lock(mutex_);
    draining_ = true;
  }
  for (auto& worker : workers_) worker->requestForce();
}

void EventLoopThreadPool::requestStop() {
  bool stop;
  {
    std::lock_guard lock(mutex_);
    if (!started_) return;
    stopping_ = true;
    stop = forwarding_ == 0;
  }
  if (stop) stopWorkers();
}

bool EventLoopThreadPool::forwardsFinished() const noexcept {
  return forwarding_ == 0;
}

void EventLoopThreadPool::join() {
  {
    std::unique_lock lock(mutex_);
    forwarded_.wait(lock, [this] { return forwardsFinished(); });
  }
  std::exception_ptr error;
  for (auto& worker : workers_) {
    try {
      worker->join();
    } catch (...) {
      if (!error) error = std::current_exception();
    }
  }
  if (error) std::rethrow_exception(error);
}
}  // namespace hp::net
