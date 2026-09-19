#include "net/event_loop.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "net/channel.h"

namespace hp::net {
namespace {
std::atomic<std::uint64_t> next_token{1};

std::uint64_t AllocateToken() {
  auto token = next_token.load(std::memory_order_relaxed);
  do {
    if (!token) throw std::overflow_error("Channel token exhausted");
  } while (!next_token.compare_exchange_weak(
      token,
      token == std::numeric_limits<std::uint64_t>::max() ? 0 : token + 1,
      std::memory_order_relaxed));
  return token;
}

void SystemCallError(const char* operation) {
  throw std::system_error(errno, std::generic_category(), operation);
}
}  // namespace

std::uint64_t EventLoop::ExchangeNextTokenForTest(std::uint64_t value) {
  return next_token.exchange(value);
}

bool EventLoop::is_in_loop_thread() const noexcept {
  return owner_ == std::this_thread::get_id();
}

void EventLoop::RequireOwner() const {
  if (!is_in_loop_thread())
    throw std::logic_error("EventLoop wrong owner thread");
}

EventLoop::EventLoop() {
  wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wake_fd_ < 0) SystemCallError("eventfd");
  try {
    wake_channel_ = std::make_unique<Channel>(*this, wake_fd_);
    wake_channel_->set_HandleWakeupEvent_callback(
        std::bind_front(&EventLoop::HandleWakeupEvent, this));
    wake_channel_->set_interest(EPOLLIN);
    counters_ = {};
  } catch (...) {
    wake_channel_.reset();
    ::close(wake_fd_);
    throw;
  }
}

EventLoop::~EventLoop() noexcept {
  assert(is_in_loop_thread());
  {
    std::lock_guard lock(mutex_);
    state_ = State::kStopped;
  }
  timers_.Clear();
  ReleaseTasks(tasks_);
  wake_channel_->Remove();
  wake_channel_.reset();
  ::close(wake_fd_);
  assert(channels_.empty());
}

void EventLoop::WakeLocked() {
  const std::uint64_t one = 1;
  for (;;) {
    auto n = ::write(wake_fd_, &one, sizeof(one));
    if (n == sizeof(one)) return;
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && errno == EAGAIN) return;
    try {
      if (n >= 0) throw std::runtime_error("short eventfd write");
      SystemCallError("eventfd write");
    } catch (...) {
      if (!failure_) failure_ = std::current_exception();
      state_ = State::kFailed;
    }
    return;  // Accepted task remains owned by loop; owner observes failure.
  }
}

void EventLoop::DrainWakeup() {
  std::uint64_t value;
  for (;;) {
    auto n = ::read(wake_fd_, &value, sizeof(value));
    if (n == sizeof(value)) continue;
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && errno == EAGAIN) return;
    if (n >= 0) throw std::runtime_error("short eventfd read");
    SystemCallError("eventfd read");
  }
}

bool EventLoop::QueueInLoop(LoopTask task) { return Enqueue(task); }

bool EventLoop::Enqueue(LoopTask& task) {
  if (!task) throw std::invalid_argument("empty EventLoop task");
  std::lock_guard lock(mutex_);
  if (state_ == State::kStopping || state_ == State::kStopped ||
      state_ == State::kFailed || outstanding_ == kTaskCapacity)
    return false;
  tasks_.push_back(std::move(task));
  ++outstanding_;
  WakeLocked();
  return true;
}

void EventLoop::ReleaseTask(LoopTask& task) {
  if (!task) return;
  task = {};  // User captures may reenter; count includes their destruction.
  std::lock_guard lock(mutex_);
  --outstanding_;
}

void EventLoop::ReleaseTasks(std::deque<LoopTask>& tasks) {
  for (auto& task : tasks) ReleaseTask(task);
  tasks.clear();
}

void EventLoop::set_HandleControl_callback(ControlCallback callback) {
  InstallControlCallback(std::move(callback));
}

void EventLoop::set_HandleWorkerControl_callback(ControlCallback callback) {
  InstallControlCallback(std::move(callback));
}

void EventLoop::InstallControlCallback(ControlCallback callback) {
  RequireOwner();
  if (polling_)
    throw std::logic_error("cannot replace control callback during dispatch");
  control_callback_ = std::move(callback);
}

bool EventLoop::failed() const {
  std::lock_guard lock(mutex_);
  return state_ == State::kFailed;
}

void EventLoop::RequestDrain(Deadline deadline) {
  std::lock_guard lock(mutex_);
  if (state_ == State::kStopped || state_ == State::kFailed ||
      control_ == Control::kForce)
    return;
  if (control_ == Control::kNone || deadline < control_deadline_)
    control_deadline_ = deadline;
  control_ = Control::kDrain;
  control_pending_ = true;
  WakeLocked();
}

void EventLoop::RequestForce() {
  std::lock_guard lock(mutex_);
  if (state_ == State::kStopped || state_ == State::kFailed) return;
  control_ = Control::kForce;
  control_pending_ = true;
  WakeLocked();
}

void EventLoop::NotifyControl() {
  std::lock_guard lock(mutex_);
  if (state_ == State::kStopped || state_ == State::kFailed) return;
  control_pending_ = true;
  WakeLocked();
}

void EventLoop::DispatchControl() {
  Control control;
  Deadline deadline;
  {
    std::lock_guard lock(mutex_);
    if (control_ == Control::kDrain &&
        timer::TimerQueue::Clock::now() >= control_deadline_) {
      control_ = Control::kForce;
      control_pending_ = true;
    }
    if (!control_pending_) return;
    control_pending_ = false;
    control = control_;
    deadline = control_deadline_;
  }
  if (control_callback_) control_callback_(control, deadline);
}

void EventLoop::RequestStop() {
  std::lock_guard lock(mutex_);
  if (state_ == State::kStopped || state_ == State::kFailed ||
      state_ == State::kStopping)
    return;
  state_ = State::kStopping;
  WakeLocked();
}

void EventLoop::Fail(std::exception_ptr error) {
  std::deque<LoopTask> cancelled;
  {
    std::lock_guard lock(mutex_);
    if (!failure_) failure_ = error;
    state_ = State::kFailed;
    failure_observed_ = true;
    cancelled.swap(tasks_);
  }
  timers_.Clear();
  ReleaseTasks(cancelled);
  // Captures are destroyed on owner, outside mutex (destructors may post).
}

void EventLoop::set_DrainClosedConnections_callback(
    std::function<void()> cleanup) {
  RequireOwner();
  if (polling_)
    throw std::logic_error("cannot replace cleanup during dispatch");
  cleanup_after_dispatch_callback_ = std::move(cleanup);
}

void EventLoop::UpdateChannel(Channel& c, std::uint32_t interest) {
  if (interest && !c.event_callback_)
    throw std::logic_error("missing Channel target");
  RequireOwner();
  if (&c.loop_ != this)
    throw std::invalid_argument("Channel belongs to another loop");
  if (c.registered()) {
    if (interest == c.interest_) return;
    epoller_.Modify(c.fd_, interest, c.token_);
    c.interest_ = interest;
    ++counters_.mods;
    return;
  }
  if (interest == 0) return;
  if (fds_.contains(c.fd_)) throw std::logic_error("fd already registered");
  const auto token = AllocateToken();
  // Allocate registry storage before ADD, roll back on failure; no throwing
  // work remains after the kernel successfully registers the fd.
  channels_.emplace(token, &c);
  try {
    fds_.emplace(c.fd_, token);
    epoller_.Add(c.fd_, interest, token);
  } catch (...) {
    fds_.erase(c.fd_);
    channels_.erase(token);
    throw;
  }
  c.token_ = token;
  c.interest_ = interest;
  ++counters_.adds;
}

void EventLoop::RemoveChannel(Channel& c) noexcept {
  assert(is_in_loop_thread());
  assert(&c.loop_ == this);
  if (&c.loop_ != this || !c.registered()) return;
  epoller_.Remove(c.fd_);
  channels_.erase(c.token_);
  fds_.erase(c.fd_);
  c.token_ = 0;
  c.interest_ = 0;
  if (&c != wake_channel_.get()) ++counters_.removes;
}

void EventLoop::Dispatch(std::uint64_t token, std::uint32_t mask) {
  const auto found = channels_.find(token);
  if (found == channels_.end()) {
    ++counters_.stale;
    return;
  }
  Channel& c = *found->second;
  if (&c == wake_channel_.get()) {
    c.event_callback_(mask);
    return;
  }
  c.revents_ = mask;
  ++counters_.dispatches;
  try {
    c.event_callback_(mask);
  } catch (...) {
    if (cleanup_after_dispatch_callback_) cleanup_after_dispatch_callback_();
    throw;
  }
  // Never access c after callback: cleanup may now destroy it and its fd owner.
  if (cleanup_after_dispatch_callback_) cleanup_after_dispatch_callback_();
}

bool EventLoop::TimersAllowed() {
  std::lock_guard lock(mutex_);
  return state_ == State::kReady || state_ == State::kRunning;
}

EventLoop::TimerId EventLoop::AddTimer(timer::TimerQueue::TimePoint deadline,
                                       LoopTask task) {
  RequireOwner();
  if (!TimersAllowed()) throw std::logic_error("timer on stopping EventLoop");
  if (!task) throw std::invalid_argument("empty timer task");
  return timers_.Add(
      deadline,
      std::bind_front(&EventLoop::DispatchTimerTask, this, std::move(task)));
}

void EventLoop::HandleWakeupEvent(std::uint32_t) { DrainWakeup(); }

void EventLoop::DispatchTimerTask(const LoopTask& task) {
  DispatchControl();
  if (!TimersAllowed()) return;
  try {
    task();
  } catch (...) {
    if (cleanup_after_dispatch_callback_) cleanup_after_dispatch_callback_();
    throw;
  }
  if (cleanup_after_dispatch_callback_) cleanup_after_dispatch_callback_();
}

bool EventLoop::RescheduleTimer(TimerId id,
                                timer::TimerQueue::TimePoint deadline) {
  RequireOwner();
  if (!TimersAllowed()) return false;
  return timers_.Reschedule(id, deadline);
}

bool EventLoop::CancelTimer(TimerId id) {
  RequireOwner();
  return timers_.Cancel(id);
}

std::size_t EventLoop::timer_count() const {
  RequireOwner();
  return timers_.size();
}

void EventLoop::PollOnce(int timeout_ms) {
  RequireOwner();
  if (polling_) throw std::logic_error("recursive EventLoop poll");
  {
    std::lock_guard lock(mutex_);
    if (state_ == State::kStopped ||
        (state_ == State::kFailed && failure_observed_))
      return;
  }
  polling_ = true;
  const auto timer_cutoff = timers_.last_id();
  std::deque<LoopTask> batch;
  try {
    DispatchControl();
    {
      std::lock_guard lock(mutex_);
      if (failure_) std::rethrow_exception(failure_);
      if (state_ == State::kStopped)
        throw std::logic_error("EventLoop stopped");
      if (!tasks_.empty() || state_ == State::kStopping || control_pending_)
        timeout_ms = 0;
    }
    if (timeout_ms < 0 || timeout_ms > 1000) timeout_ms = 1000;
    if (auto deadline = timers_.next_deadline()) {
      const auto remaining = *deadline - timer::TimerQueue::Clock::now();
      if (remaining <= timer::TimerQueue::Clock::duration::zero()) {
        timeout_ms = 0;
      } else {
        const auto millis =
            std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
        if (millis < timeout_ms) timeout_ms = static_cast<int>(millis);
      }
    }
    {
      std::lock_guard lock(mutex_);
      if (control_ == Control::kDrain) {
        const auto remaining =
            control_deadline_ - timer::TimerQueue::Clock::now();
        const auto millis =
            std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
        timeout_ms = static_cast<int>(
            std::max<std::int64_t>(0,
                                   std::min<std::int64_t>(timeout_ms, millis)));
      }
    }
    const auto events = epoller_.Wait(timeout_ms);
    DispatchControl();
    for (const auto& event : events) {
      DispatchControl();
      Dispatch(event.data.u64, event.events);
    }
    {
      std::lock_guard lock(mutex_);
      if (failure_) std::rethrow_exception(failure_);
      batch.swap(tasks_);
    }
    for (auto& task : batch) {
      DispatchControl();
      {
        std::lock_guard lock(mutex_);
        if (failure_) std::rethrow_exception(failure_);
      }
      task();
      ReleaseTask(task);
    }
    DispatchControl();
    if (TimersAllowed())
      timers_.RunDue(timer::TimerQueue::Clock::now(), timer_cutoff);
    if (!TimersAllowed()) timers_.Clear();
    {
      std::lock_guard lock(mutex_);
      if (state_ == State::kStopping && tasks_.empty())
        state_ = State::kStopped;
    }
  } catch (...) {
    Fail(std::current_exception());
    ReleaseTasks(batch);
    polling_ = false;
    std::exception_ptr error;
    {
      std::lock_guard lock(mutex_);
      error = failure_;
    }
    std::rethrow_exception(error);
  }
  polling_ = false;
}

void EventLoop::Loop() {
  RequireOwner();
  {
    std::lock_guard lock(mutex_);
    if (state_ == State::kRunning || state_ == State::kStopped)
      throw std::logic_error("EventLoop cannot restart");
    if (failure_) std::rethrow_exception(failure_);
    if (state_ == State::kReady) state_ = State::kRunning;
  }
  for (;;) {
    PollOnce(-1);
    std::exception_ptr error;
    {
      std::lock_guard lock(mutex_);
      if (state_ == State::kStopped) return;
      error = failure_;
    }
    if (error) {
      Fail(error);
      std::rethrow_exception(error);
    }
  }
}
}  // namespace hp::net
