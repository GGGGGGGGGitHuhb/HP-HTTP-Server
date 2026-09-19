#include "net/event_loop_thread.h"

#include <cassert>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "base/logger.h"

namespace hp::net {
EventLoopThread::~EventLoopThread() noexcept {
  assert(std::this_thread::get_id() != worker_id_);
  try {
    RequestStop();
    Join();
  } catch (const std::exception& e) {
    hp::base::error(e.what());
  } catch (...) {
    hp::base::error("unobserved EventLoopThread failure");
  }
}

void EventLoopThread::LoopBoundTask::RunInLoop() const { task(*target); }

void EventLoopThread::Start(InitCallback init, CleanupCallback cleanup) {
  std::unique_lock lock(mutex_);
  if (started_) throw std::logic_error("EventLoopThread already started");
  started_ = true;
  try {
    thread_ = std::thread(&EventLoopThread::Run,
                          this,
                          std::move(init),
                          std::move(cleanup));
  } catch (...) {
    ready_flag_ = true;
    throw;
  }
  ready_.wait(lock, [this] { return ready_flag_; });
  if (!startup_succeeded_) {
    lock.unlock();
    Join();
  }
}

bool EventLoopThread::Post(LoopTask task) {
  if (!task) throw std::invalid_argument("empty EventLoopThread task");
  // Reject before allocating. Allocate an empty binding before moving user
  // captures; queued is destroyed after the lock on every failure path.
  EventLoop::LoopTask queued;
  std::lock_guard lock(mutex_);
  if (!loop_) return false;
  EventLoop* target = loop_;
  using BoundTask = decltype(std::bind_front(&LoopBoundTask::RunInLoop,
                                             LoopBoundTask{target, {}}));
  static_assert(std::is_nothrow_move_assignable_v<BoundTask>);
  static_assert(std::is_nothrow_move_constructible_v<BoundTask>);
  queued =
      std::bind_front(&LoopBoundTask::RunInLoop, LoopBoundTask{target, {}});
  *queued.target<BoundTask>() =
      std::bind_front(&LoopBoundTask::RunInLoop,
                      LoopBoundTask{target, std::move(task)});
  return target->Enqueue(queued);
}

void EventLoopThread::RequestDrain(EventLoop::Deadline deadline) {
  std::lock_guard lock(mutex_);
  if (loop_) loop_->RequestDrain(deadline);
}

void EventLoopThread::RequestForce() {
  std::lock_guard lock(mutex_);
  if (loop_) loop_->RequestForce();
}

void EventLoopThread::RequestStop() {
  std::lock_guard lock(mutex_);
  if (!started_) return;
  stop_ = true;
  if (loop_) loop_->RequestStop();
}

void EventLoopThread::Join() {
  {
    std::lock_guard lock(mutex_);
    if (std::this_thread::get_id() == worker_id_)
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

void EventLoopThread::Run(InitCallback init, CleanupCallback cleanup) noexcept {
  std::exception_ptr error;
  {
    std::lock_guard lock(mutex_);
    worker_id_ = std::this_thread::get_id();
  }
  try {
    EventLoop loop;
    try {
      if (init) init(loop);
      {
        std::lock_guard lock(mutex_);
        if (stop_) loop.RequestStop();
        loop_ = &loop;
        startup_succeeded_ = true;
        ready_flag_ = true;
      }
      ready_.notify_all();
      loop.Loop();
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
    ready_flag_ = true;
  }
  ready_.notify_all();
}
}  // namespace hp::net
