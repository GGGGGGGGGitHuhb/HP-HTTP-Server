#include "net/event_loop.h"
#include "net/channel.h"
#include <cassert>
#include <limits>
#include <stdexcept>
#include <utility>

namespace hp::net {
namespace {
// Process-wide monotonic identity; this S1 API is single-threaded.
std::uint64_t next_token = 1;

std::uint64_t allocate_token() {
    if (next_token == 0)
        throw std::overflow_error("Channel token exhausted");
    const auto token = next_token;
    next_token = token == std::numeric_limits<std::uint64_t>::max() ? 0 : token + 1;
    return token;
}
}

EventLoop::~EventLoop() noexcept {
    assert(channels_.empty());
}

void EventLoop::set_after_dispatch(std::function<void()> cleanup) {
    if (polling_)
        throw std::logic_error("cannot replace cleanup during dispatch");
    after_dispatch_ = std::move(cleanup);
}

void EventLoop::update_channel(Channel& c, std::uint32_t interest) {
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
    assert(&c.loop_ == this);
    if (&c.loop_ != this || !c.registered())
        return;
    epoller_.remove(c.fd_);
    channels_.erase(c.token_);
    fds_.erase(c.fd_);
    c.token_ = 0;
    c.interest_ = 0;
    ++counters_.removes;
}

void EventLoop::dispatch(std::uint64_t token, std::uint32_t mask) {
    const auto found = channels_.find(token);
    if (found == channels_.end()) {
        ++counters_.stale;
        return;
    }
    Channel& c = *found->second;
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
    if (polling_)
        throw std::logic_error("recursive EventLoop poll");
    polling_ = true;
    try {
        const auto events = epoller_.wait(timeout_ms);
        for (const auto& event : events)
            dispatch(event.data.u64, event.events);
    } catch (...) {
        polling_ = false;
        throw;
    }
    polling_ = false;
}

[[noreturn]] void EventLoop::loop() {
    while (true)
        poll_once(-1);
}
}
