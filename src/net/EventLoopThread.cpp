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
    requestStop();
    join();
  } catch (const std::exception& e) {
    hp::base::error(e.what());
  } catch (...) {
    hp::base::error("unobserved EventLoopThread failure");
  }
}

void EventLoopThread::LoopBoundTask::runInLoop() const { task(*target); }

void EventLoopThread::start(InitCallback init, CleanupCallback cleanup) {
  std::unique_lock lock(mutex_);
  if (started_) throw std::logic_error("EventLoopThread already started");
  started_ = true;
  try {
    thread_ = std::thread(&EventLoopThread::run,
                          this,
                          std::move(init),
                          std::move(cleanup));
  } catch (...) {
    readyFlag_ = true;
    throw;
  }
  ready_.wait(lock, [this] { return isReady(); });
  if (!startupSucceeded_) {
    lock.unlock();
    join();
  }
}

bool EventLoopThread::post(TaskCallback task) {
  if (!task) throw std::invalid_argument("empty EventLoopThread task");
  // Empty slot and queue allocations precede the noexcept task transfer.
  // Both owners precede the lock, so user captures are released after unlock.
  std::shared_ptr<LoopBoundTask> slot;
  EventLoop::TaskCallback queued;
  std::lock_guard lock(mutex_);
  if (!loop_) return false;
  slot = std::make_shared<LoopBoundTask>(LoopBoundTask{loop_, {}});
  queued = makeQueuedTask(slot);
  slot->task = std::move(task);
  // Enqueue may wake the owner immediately: queued must be the sole owner.
  slot.reset();
  return loop_->enqueue(queued);
}

EventLoop::TaskCallback EventLoopThread::makeQueuedTask(
    const std::shared_ptr<LoopBoundTask>& slot) {
  return [slot] { slot->runInLoop(); };
}

bool EventLoopThread::isReady() const noexcept { return readyFlag_; }

void EventLoopThread::requestDrain(EventLoop::Deadline deadline) {
  std::lock_guard lock(mutex_);
  if (loop_) loop_->requestDrain(deadline);
}

void EventLoopThread::requestForce() {
  std::lock_guard lock(mutex_);
  if (loop_) loop_->requestForce();
}

void EventLoopThread::requestStop() {
  std::lock_guard lock(mutex_);
  if (!started_) return;
  stop_ = true;
  if (loop_) loop_->requestStop();
}

void EventLoopThread::join() {
  {
    std::lock_guard lock(mutex_);
    if (std::this_thread::get_id() == workerId_)
      throw std::logic_error("EventLoopThread self join");
  }
  if (!thread_.joinable()) return;
  thread_.join();
  std::exception_ptr error;
  {
    std::lock_guard lock(mutex_);
    error = std::exchange(failure_, {});
  }
  if (error) std::rethrow_exception(error);
}

void EventLoopThread::run(InitCallback init, CleanupCallback cleanup) noexcept {
  std::exception_ptr error;
  {
    std::lock_guard lock(mutex_);
    workerId_ = std::this_thread::get_id();
  }
  try {
    EventLoop loop;
    try {
      if (init) init(loop);
      {
        std::lock_guard lock(mutex_);
        if (stop_) loop.requestStop();
        loop_ = &loop;
        startupSucceeded_ = true;
        readyFlag_ = true;
      }
      ready_.notify_all();
      loop.loop();
    } catch (...) {
      error = std::current_exception();
    }
    {
      std::lock_guard lock(mutex_);
      loop_ = nullptr;
    }
    try {
      if (cleanup) cleanup(loop);
    } catch (...) {
      if (!error) error = std::current_exception();
    }
  } catch (...) {
    if (!error) error = std::current_exception();
  }
  {
    std::lock_guard lock(mutex_);
    failure_ = error;
    readyFlag_ = true;
  }
  ready_.notify_all();
}
}  // namespace hp::net
