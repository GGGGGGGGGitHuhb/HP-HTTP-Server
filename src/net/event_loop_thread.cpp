#include "net/event_loop_thread.h"

#include <cassert>
#include <stdexcept>
#include <utility>

#include "base/logger.h"

namespace hp::net {
EventLoopThread::~EventLoopThread() noexcept {
  assert(std::this_thread::get_id() != worker_id_);
  try {
    request_stop();
    join();
  } catch (const std::exception& e) {
    hp::base::error(e.what());
  } catch (...) {
    hp::base::error("unobserved EventLoopThread failure");
  }
}

void EventLoopThread::start(Callback init, Callback cleanup) {
  std::unique_lock lock(mutex_);
  if (started_) throw std::logic_error("EventLoopThread already started");
  started_ = true;
  try {
    thread_ = std::thread(
        [this, init = std::move(init), cleanup = std::move(cleanup)]() mutable {
          run(std::move(init), std::move(cleanup));
        });
  } catch (...) {
    ready_flag_ = true;
    throw;
  }
  ready_.wait(lock, [this] { return ready_flag_; });
  if (!startup_succeeded_) {
    lock.unlock();
    join();
  }
}

bool EventLoopThread::post(Callback task) {
  if (!task) throw std::invalid_argument("empty EventLoopThread task");
  // Rejection leaves the capture here until after forwarding unlocks. On
  // acceptance enqueue moves sole ownership to the loop, including
  // cancellation.
  EventLoop::Task queued;
  std::lock_guard lock(mutex_);
  if (!loop_) return false;
  EventLoop* target = loop_;
  queued = [target, task = std::move(task)] { task(*target); };
  return target->enqueue(queued);
}

void EventLoopThread::request_drain(EventLoop::Deadline deadline) {
  std::lock_guard lock(mutex_);
  if (loop_) loop_->request_drain(deadline);
}

void EventLoopThread::request_force() {
  std::lock_guard lock(mutex_);
  if (loop_) loop_->request_force();
}

void EventLoopThread::request_stop() {
  std::lock_guard lock(mutex_);
  if (!started_) return;
  stop_ = true;
  if (loop_) loop_->request_stop();
}

void EventLoopThread::join() {
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

void EventLoopThread::run(Callback init, Callback cleanup) noexcept {
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
        if (stop_) loop.request_stop();
        loop_ = &loop;
        startup_succeeded_ = true;
        ready_flag_ = true;
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
    ready_flag_ = true;
  }
  ready_.notify_all();
}
}  // namespace hp::net
