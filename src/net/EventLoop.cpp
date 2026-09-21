#include "net/EventLoop.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "net/Channel.h"

namespace hp::net {
namespace {
std::atomic<std::uint64_t> nextToken{1};

std::uint64_t allocateToken() {
  auto token = nextToken.load(std::memory_order_relaxed);
  do {
    if (!token) throw std::overflow_error("Channel token exhausted");
  } while (!nextToken.compare_exchange_weak(
      token,
      token == std::numeric_limits<std::uint64_t>::max() ? 0 : token + 1,
      std::memory_order_relaxed));
  return token;
}

void systemCallError(const char* operation) {
  throw std::system_error(errno, std::generic_category(), operation);
}
}  // namespace

std::uint64_t EventLoop::exchangeNextTokenForTest(std::uint64_t value) {
  return nextToken.exchange(value);
}

bool EventLoop::isInLoopThread() const noexcept {
  return owner_ == std::this_thread::get_id();
}

void EventLoop::requireOwner() const {
  if (!isInLoopThread()) throw std::logic_error("EventLoop wrong owner thread");
}

EventLoop::EventLoop() {
  wakeFd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wakeFd_ < 0) systemCallError("eventfd");
  try {
    wakeChannel_ = std::make_unique<Channel>(*this, wakeFd_);
    wakeChannel_->registerEventCallback(
        [this](std::uint32_t events) { handleWakeupEvent(events); });
    wakeChannel_->setInterest(EPOLLIN);
    counters_ = {};
  } catch (...) {
    wakeChannel_.reset();
    ::close(wakeFd_);
    throw;
  }
}

EventLoop::~EventLoop() noexcept {
  assert(isInLoopThread());
  {
    std::lock_guard lock(mutex_);
    state_ = State::kStopped;
  }
  timers_.clear();
  releaseTasks(tasks_);
  wakeChannel_->remove();
  wakeChannel_.reset();
  ::close(wakeFd_);
  assert(channels_.empty());
}

void EventLoop::wakeLocked() {
  const std::uint64_t one = 1;
  for (;;) {
    auto n = ::write(wakeFd_, &one, sizeof(one));
    if (n == sizeof(one)) return;
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && errno == EAGAIN) return;
    try {
      if (n >= 0) throw std::runtime_error("short eventfd write");
      systemCallError("eventfd write");
    } catch (...) {
      if (!failure_) failure_ = std::current_exception();
      state_ = State::kFailed;
    }
    return;  // Accepted task remains owned by loop; owner observes failure.
  }
}

void EventLoop::drainWakeup() {
  std::uint64_t value;
  for (;;) {
    auto n = ::read(wakeFd_, &value, sizeof(value));
    if (n == sizeof(value)) continue;
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && errno == EAGAIN) return;
    if (n >= 0) throw std::runtime_error("short eventfd read");
    systemCallError("eventfd read");
  }
}

bool EventLoop::queueInLoop(TaskCallback task) { return enqueue(task); }

bool EventLoop::enqueue(TaskCallback& task) {
  if (!task) throw std::invalid_argument("empty EventLoop task");
  std::lock_guard lock(mutex_);
  if (state_ == State::kStopping || state_ == State::kStopped ||
      state_ == State::kFailed || outstanding_ == kTaskCapacity)
    return false;
  tasks_.push_back(std::move(task));
  ++outstanding_;
  wakeLocked();
  return true;
}

void EventLoop::releaseTask(TaskCallback& task) {
  if (!task) return;
  task = {};  // User captures may reenter; count includes their destruction.
  std::lock_guard lock(mutex_);
  --outstanding_;
}

void EventLoop::releaseTasks(std::deque<TaskCallback>& tasks) {
  for (auto& task : tasks) releaseTask(task);
  tasks.clear();
}

void EventLoop::registerControlCallback(ControlCallback controlCallback) {
  requireOwner();
  if (polling_)
    throw std::logic_error("cannot replace control callback during dispatch");
  setControlCallback(std::move(controlCallback));
}

bool EventLoop::failed() const {
  std::lock_guard lock(mutex_);
  return state_ == State::kFailed;
}

void EventLoop::requestDrain(Deadline deadline) {
  std::lock_guard lock(mutex_);
  if (state_ == State::kStopped || state_ == State::kFailed ||
      control_ == Control::kForce)
    return;
  if (control_ == Control::kNone || deadline < controlDeadline_)
    controlDeadline_ = deadline;
  control_ = Control::kDrain;
  controlPending_ = true;
  wakeLocked();
}

void EventLoop::requestForce() {
  std::lock_guard lock(mutex_);
  if (state_ == State::kStopped || state_ == State::kFailed) return;
  control_ = Control::kForce;
  controlPending_ = true;
  wakeLocked();
}

void EventLoop::notifyControl() {
  std::lock_guard lock(mutex_);
  if (state_ == State::kStopped || state_ == State::kFailed) return;
  controlPending_ = true;
  wakeLocked();
}

void EventLoop::dispatchControl() {
  Control control;
  Deadline deadline;
  {
    std::lock_guard lock(mutex_);
    if (control_ == Control::kDrain &&
        timer::TimerQueue::Clock::now() >= controlDeadline_) {
      control_ = Control::kForce;
      controlPending_ = true;
    }
    if (!controlPending_) return;
    controlPending_ = false;
    control = control_;
    deadline = controlDeadline_;
  }
  if (controlCallback_) controlCallback_(control, deadline);
}

void EventLoop::requestStop() {
  std::lock_guard lock(mutex_);
  if (state_ == State::kStopped || state_ == State::kFailed ||
      state_ == State::kStopping)
    return;
  state_ = State::kStopping;
  wakeLocked();
}

void EventLoop::fail(std::exception_ptr error) {
  std::deque<TaskCallback> cancelled;
  {
    std::lock_guard lock(mutex_);
    if (!failure_) failure_ = error;
    state_ = State::kFailed;
    failureObserved_ = true;
    cancelled.swap(tasks_);
  }
  timers_.clear();
  releaseTasks(cancelled);
  // Captures are destroyed on owner, outside mutex (destructors may post).
}

void EventLoop::registerCleanupCallback(CleanupCallback cleanupCallback) {
  requireOwner();
  if (polling_)
    throw std::logic_error("cannot replace cleanup during dispatch");
  setCleanupCallback(std::move(cleanupCallback));
}

void EventLoop::updateChannel(Channel& c, std::uint32_t interest) {
  if (interest && !c.eventCallback_)
    throw std::logic_error("missing Channel target");
  requireOwner();
  if (&c.loop_ != this)
    throw std::invalid_argument("Channel belongs to another loop");
  if (c.registered()) {
    if (interest == c.interest_) return;
    epoller_.modify(c.fd_, interest, c.token_);
    c.interest_ = interest;
    ++counters_.mods;
    return;
  }
  if (interest == 0) return;
  if (fds_.contains(c.fd_)) throw std::logic_error("fd already registered");
  const auto token = allocateToken();
  // Allocate registry storage before ADD, roll back on failure; no throwing
  // work remains after the kernel successfully registers the fd.
  channels_.emplace(token, &c);
  try {
    fds_.emplace(c.fd_, token);
    epoller_.add(c.fd_, interest, token);
  } catch (...) {
    fds_.erase(c.fd_);
    channels_.erase(token);
    throw;
  }
  c.token_ = token;
  c.interest_ = interest;
  ++counters_.adds;
}

void EventLoop::removeChannel(Channel& c) noexcept {
  assert(isInLoopThread());
  assert(&c.loop_ == this);
  if (&c.loop_ != this || !c.registered()) return;
  epoller_.remove(c.fd_);
  channels_.erase(c.token_);
  fds_.erase(c.fd_);
  c.token_ = 0;
  c.interest_ = 0;
  if (&c != wakeChannel_.get()) ++counters_.removes;
}

void EventLoop::dispatch(std::uint64_t token, std::uint32_t mask) {
  const auto found = channels_.find(token);
  if (found == channels_.end()) {
    ++counters_.stale;
    return;
  }
  Channel& c = *found->second;
  if (&c == wakeChannel_.get()) {
    c.eventCallback_(mask);
    return;
  }
  c.revents_ = mask;
  ++counters_.dispatches;
  try {
    c.eventCallback_(mask);
  } catch (...) {
    if (cleanupCallback_) cleanupCallback_();
    throw;
  }
  // Never access c after callback: cleanup may now destroy it and its fd owner.
  if (cleanupCallback_) cleanupCallback_();
}

bool EventLoop::timersAllowed() {
  std::lock_guard lock(mutex_);
  return state_ == State::kReady || state_ == State::kRunning;
}

EventLoop::TimerId EventLoop::addTimer(timer::TimerQueue::TimePoint deadline,
                                       TaskCallback task) {
  requireOwner();
  if (!timersAllowed()) throw std::logic_error("timer on stopping EventLoop");
  if (!task) throw std::invalid_argument("empty timer task");
  return timers_.add(deadline, [this, task = std::move(task)]() {
    dispatchTimerTask(task);
  });
}

void EventLoop::handleWakeupEvent(std::uint32_t) { drainWakeup(); }

void EventLoop::dispatchTimerTask(const TaskCallback& task) {
  dispatchControl();
  if (!timersAllowed()) return;
  try {
    task();
  } catch (...) {
    if (cleanupCallback_) cleanupCallback_();
    throw;
  }
  if (cleanupCallback_) cleanupCallback_();
}

bool EventLoop::rescheduleTimer(TimerId id,
                                timer::TimerQueue::TimePoint deadline) {
  requireOwner();
  if (!timersAllowed()) return false;
  return timers_.reschedule(id, deadline);
}

bool EventLoop::cancelTimer(TimerId id) {
  requireOwner();
  return timers_.cancel(id);
}

std::size_t EventLoop::timerCount() const {
  requireOwner();
  return timers_.size();
}

void EventLoop::pollOnce(int timeoutMs) {
  requireOwner();
  if (polling_) throw std::logic_error("recursive EventLoop poll");
  {
    std::lock_guard lock(mutex_);
    if (state_ == State::kStopped ||
        (state_ == State::kFailed && failureObserved_))
      return;
  }
  polling_ = true;
  const auto timerCutoff = timers_.lastId();
  std::deque<TaskCallback> batch;
  try {
    dispatchControl();
    {
      std::lock_guard lock(mutex_);
      if (failure_) std::rethrow_exception(failure_);
      if (state_ == State::kStopped)
        throw std::logic_error("EventLoop stopped");
      if (!tasks_.empty() || state_ == State::kStopping || controlPending_)
        timeoutMs = 0;
    }
    if (timeoutMs < 0 || timeoutMs > 1000) timeoutMs = 1000;
    if (auto deadline = timers_.nextDeadline()) {
      const auto remaining = *deadline - timer::TimerQueue::Clock::now();
      if (remaining <= timer::TimerQueue::Clock::duration::zero()) {
        timeoutMs = 0;
      } else {
        const auto millis =
            std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
        if (millis < timeoutMs) timeoutMs = static_cast<int>(millis);
      }
    }
    {
      std::lock_guard lock(mutex_);
      if (control_ == Control::kDrain) {
        const auto remaining =
            controlDeadline_ - timer::TimerQueue::Clock::now();
        const auto millis =
            std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
        timeoutMs = static_cast<int>(
            std::max<std::int64_t>(0,
                                   std::min<std::int64_t>(timeoutMs, millis)));
      }
    }
    const auto events = epoller_.wait(timeoutMs);
    dispatchControl();
    for (const auto& event : events) {
      dispatchControl();
      dispatch(event.data.u64, event.events);
    }
    {
      std::lock_guard lock(mutex_);
      if (failure_) std::rethrow_exception(failure_);
      batch.swap(tasks_);
    }
    for (auto& task : batch) {
      dispatchControl();
      {
        std::lock_guard lock(mutex_);
        if (failure_) std::rethrow_exception(failure_);
      }
      task();
      releaseTask(task);
    }
    dispatchControl();
    if (timersAllowed())
      timers_.runDue(timer::TimerQueue::Clock::now(), timerCutoff);
    if (!timersAllowed()) timers_.clear();
    {
      std::lock_guard lock(mutex_);
      if (state_ == State::kStopping && tasks_.empty())
        state_ = State::kStopped;
    }
  } catch (...) {
    fail(std::current_exception());
    releaseTasks(batch);
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

void EventLoop::loop() {
  requireOwner();
  {
    std::lock_guard lock(mutex_);
    if (state_ == State::kRunning || state_ == State::kStopped)
      throw std::logic_error("EventLoop cannot restart");
    if (failure_) std::rethrow_exception(failure_);
    if (state_ == State::kReady) state_ = State::kRunning;
  }
  for (;;) {
    pollOnce(-1);
    std::exception_ptr error;
    {
      std::lock_guard lock(mutex_);
      if (state_ == State::kStopped) return;
      error = failure_;
    }
    if (error) {
      fail(error);
      std::rethrow_exception(error);
    }
  }
}
}  // namespace hp::net
