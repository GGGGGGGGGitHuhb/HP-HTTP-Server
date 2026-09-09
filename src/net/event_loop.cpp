#include "net/event_loop.h"
#include "net/channel.h"
#include <cassert>
#include <atomic>
#include <cerrno>
#include <system_error>
#include <sys/eventfd.h>
#include <unistd.h>
#include <limits>
#include <stdexcept>
#include <utility>

namespace hp::net {
namespace {
std::atomic<std::uint64_t> next_token{1};

std::uint64_t allocate_token() {
    auto token = next_token.load(std::memory_order_relaxed);
    do {
        if (!token)
            throw std::overflow_error("Channel token exhausted");
    } while (!next_token.compare_exchange_weak(
        token, token == std::numeric_limits<std::uint64_t>::max() ? 0 : token + 1,
        std::memory_order_relaxed));
    return token;
}

void syscall_error(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}
} // namespace

std::uint64_t EventLoop::exchange_next_token_for_test(std::uint64_t value) {
    return next_token.exchange(value);
}

bool EventLoop::is_in_loop_thread() const noexcept {
    return owner_ == std::this_thread::get_id();
}

void EventLoop::require_owner() const {
    if (!is_in_loop_thread())
        throw std::logic_error("EventLoop wrong owner thread");
}

EventLoop::EventLoop() {
    wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd_ < 0)
        syscall_error("eventfd");
    try {
        wake_channel_ =
            std::make_unique<Channel>(*this, wake_fd_, [this](std::uint32_t) { drain_wakeup(); });
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
        state_ = State::Stopped;
    }
    tasks_.clear();
    wake_channel_->remove();
    wake_channel_.reset();
    ::close(wake_fd_);
    assert(channels_.empty());
}

void EventLoop::wake_locked() {
    const std::uint64_t one = 1;
    for (;;) {
        auto n = ::write(wake_fd_, &one, sizeof(one));
        if (n == sizeof(one))
            return;
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN)
            return;
        try {
            if (n >= 0)
                throw std::runtime_error("short eventfd write");
            syscall_error("eventfd write");
        } catch (...) {
            if (!failure_)
                failure_ = std::current_exception();
            state_ = State::Failed;
        }
        return; // Accepted task remains owned by loop; owner observes failure.
    }
}

void EventLoop::drain_wakeup() {
    std::uint64_t value;
    for (;;) {
        auto n = ::read(wake_fd_, &value, sizeof(value));
        if (n == sizeof(value))
            continue;
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN)
            return;
        if (n >= 0)
            throw std::runtime_error("short eventfd read");
        syscall_error("eventfd read");
    }
}

bool EventLoop::queue_in_loop(Task task) {
    return enqueue(task);
}

bool EventLoop::enqueue(Task& task) {
    if (!task)
        throw std::invalid_argument("empty EventLoop task");
    std::lock_guard lock(mutex_);
    if (state_ == State::Stopping || state_ == State::Stopped || state_ == State::Failed)
        return false;
    tasks_.push_back(std::move(task));
    wake_locked();
    return true;
}

void EventLoop::request_stop() {
    std::lock_guard lock(mutex_);
    if (state_ == State::Stopped || state_ == State::Failed || state_ == State::Stopping)
        return;
    state_ = State::Stopping;
    wake_locked();
}

void EventLoop::fail(std::exception_ptr error) {
    std::deque<Task> cancelled;
    {
        std::lock_guard lock(mutex_);
        if (!failure_)
            failure_ = error;
        state_ = State::Failed;
        failure_observed_ = true;
        cancelled.swap(tasks_);
    }
    // Captures are destroyed on owner, outside mutex (destructors may post).
}

void EventLoop::set_after_dispatch(std::function<void()> cleanup) {
    require_owner();
    if (polling_)
        throw std::logic_error("cannot replace cleanup during dispatch");
    after_dispatch_ = std::move(cleanup);
}

void EventLoop::update_channel(Channel& c, std::uint32_t interest) {
    require_owner();
    if (&c.loop_ != this)
        throw std::invalid_argument("Channel belongs to another loop");
    if (c.registered()) {
        if (interest == c.interest_)
            return;
        epoller_.modify(c.fd_, interest, c.token_);
        c.interest_ = interest;
        ++counters_.mods;
        return;
    }
    if (interest == 0)
        return;
    if (fds_.contains(c.fd_))
        throw std::logic_error("fd already registered");
    const auto token = allocate_token();
    // Allocate registry storage before ADD, roll back on failure; no throwing work
    // remains after the kernel successfully registers the fd.
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

void EventLoop::remove_channel(Channel& c) noexcept {
    assert(is_in_loop_thread());
    assert(&c.loop_ == this);
    if (&c.loop_ != this || !c.registered())
        return;
    epoller_.remove(c.fd_);
    channels_.erase(c.token_);
    fds_.erase(c.fd_);
    c.token_ = 0;
    c.interest_ = 0;
    if (&c != wake_channel_.get())
        ++counters_.removes;
}

void EventLoop::dispatch(std::uint64_t token, std::uint32_t mask) {
    const auto found = channels_.find(token);
    if (found == channels_.end()) {
        ++counters_.stale;
        return;
    }
    Channel& c = *found->second;
    if (&c == wake_channel_.get()) {
        c.callback_(mask);
        return;
    }
    c.revents_ = mask;
    ++counters_.dispatches;
    try {
        c.callback_(mask);
    } catch (...) {
        if (after_dispatch_)
            after_dispatch_();
        throw;
    }
    // Never access c after callback: cleanup may now destroy it and its fd owner.
    if (after_dispatch_)
        after_dispatch_();
}

void EventLoop::poll_once(int timeout_ms) {
    require_owner();
    if (polling_)
        throw std::logic_error("recursive EventLoop poll");
    {
        std::lock_guard lock(mutex_);
        if (state_ == State::Stopped || (state_ == State::Failed && failure_observed_))
            return;
    }
    polling_ = true;
    std::deque<Task> batch;
    try {
        {
            std::lock_guard lock(mutex_);
            if (failure_)
                std::rethrow_exception(failure_);
            if (state_ == State::Stopped)
                throw std::logic_error("EventLoop stopped");
            if (!tasks_.empty() || state_ == State::Stopping)
                timeout_ms = 0;
        }
        if (timeout_ms < 0 || timeout_ms > 1000)
            timeout_ms = 1000;
        const auto events = epoller_.wait(timeout_ms);
        for (const auto& event : events)
            dispatch(event.data.u64, event.events);
        {
            std::lock_guard lock(mutex_);
            if (failure_)
                std::rethrow_exception(failure_);
            batch.swap(tasks_);
        }
        for (auto& task : batch) {
            {
                std::lock_guard lock(mutex_);
                if (failure_)
                    std::rethrow_exception(failure_);
            }
            task();
            task = {};
        }
        {
            std::lock_guard lock(mutex_);
            if (state_ == State::Stopping && tasks_.empty())
                state_ = State::Stopped;
        }
    } catch (...) {
        fail(std::current_exception());
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
    require_owner();
    {
        std::lock_guard lock(mutex_);
        if (state_ == State::Running || state_ == State::Stopped)
            throw std::logic_error("EventLoop cannot restart");
        if (failure_)
            std::rethrow_exception(failure_);
        if (state_ == State::Ready)
            state_ = State::Running;
    }
    for (;;) {
        poll_once(-1);
        std::exception_ptr error;
        {
            std::lock_guard lock(mutex_);
            if (state_ == State::Stopped)
                return;
            error = failure_;
        }
        if (error) {
            fail(error);
            std::rethrow_exception(error);
        }
    }
}
} // namespace hp::net
