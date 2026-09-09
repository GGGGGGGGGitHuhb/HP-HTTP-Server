#include "net/event_loop_thread_pool.h"
#include "base/logger.h"
#include <stdexcept>
#include <utility>

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

EventLoopThreadPool::~EventLoopThreadPool() noexcept {
    try {
        request_stop();
        join();
    } catch (const std::exception& error) {
        base::error(error.what());
    } catch (...) {
        base::error("unobserved EventLoopThreadPool failure");
    }
}

void EventLoopThreadPool::start(std::size_t count, Callback init, Callback cleanup) {
    {
        std::lock_guard lock(mutex_);
        if (started_)
            throw std::logic_error("pool already started");
        if (count > 64)
            throw std::invalid_argument("worker count exceeds 64");
        started_ = true;
        workers_.reserve(count);
        outstanding_.resize(count);
        for (std::size_t i = 0; i < count; ++i)
            workers_.push_back(std::make_unique<EventLoopThread>());
    }
    try {
        for (std::size_t i = 0; i < count; ++i) {
            workers_[i]->start(
                [init, i](EventLoop& loop) {
                    if (init)
                        init(i, loop);
                },
                [this, cleanup, i](EventLoop& loop) {
                    // Planned drain may finish one owner before its peers. A fatal
                    // still stops every peer without using the ordinary queue.
                    bool planned;
                    {
                        std::lock_guard lock(mutex_);
                        planned = draining_;
                    }
                    if (!planned || loop.failed())
                        request_stop();
                    if (cleanup)
                        cleanup(i, loop);
                });
        }
        bool stop;
        {
            std::lock_guard lock(mutex_);
            ready_ = true;
            stop = stopping_;
        }
        if (stop)
            stop_workers();
    } catch (...) {
        auto error = std::current_exception();
        request_stop();
        try {
            join();
        } catch (...) {
        }
        std::rethrow_exception(error);
    }
}

bool EventLoopThreadPool::post(std::size_t index, EventLoopThread::Callback task) {
    if (!task)
        throw std::invalid_argument("empty pool task");
    auto ticket = std::make_shared<Ticket>(Ticket{*this, index});
    auto* reservation = ticket.get();
    EventLoopThread::Callback queued = [ticket = std::move(ticket),
                                        task = std::move(task)](EventLoop& loop) {
        task(loop);
    };
    {
        std::lock_guard lock(mutex_);
        if (!ready_ || stopping_ || index >= workers_.size() ||
            outstanding_[index] == task_capacity)
            return false;
        ++outstanding_[index];
        reservation->armed = true;
        ++forwarding_;
    }
    // Stop records its cutoff now, but signals workers only after all accepted
    // forwards finish. No user capture is released under the pool mutex.
    try {
        const bool accepted = workers_[index]->post(std::move(queued));
        finish_forward();
        return accepted;
    } catch (...) {
        finish_forward();
        throw;
    }
}

void EventLoopThreadPool::finish_forward() noexcept {
    bool stop;
    {
        std::lock_guard lock(mutex_);
        --forwarding_;
        stop = stopping_ && forwarding_ == 0;
        forwarded_.notify_all();
    }
    if (stop)
        stop_workers();
}

void EventLoopThreadPool::stop_workers() {
    for (auto& worker : workers_)
        worker->request_stop();
}

void EventLoopThreadPool::request_drain(EventLoop::Deadline deadline) {
    {
        std::lock_guard lock(mutex_);
        draining_ = true;
    }
    for (auto& worker : workers_)
        worker->request_drain(deadline);
}

void EventLoopThreadPool::request_force() {
    {
        std::lock_guard lock(mutex_);
        draining_ = true;
    }
    for (auto& worker : workers_)
        worker->request_force();
}

void EventLoopThreadPool::request_stop() {
    bool stop;
    {
        std::lock_guard lock(mutex_);
        if (!started_)
            return;
        stopping_ = true;
        stop = forwarding_ == 0;
    }
    if (stop)
        stop_workers();
}

void EventLoopThreadPool::join() {
    {
        std::unique_lock lock(mutex_);
        forwarded_.wait(lock, [this] {
            return forwarding_ == 0;
        });
    }
    std::exception_ptr error;
    for (auto& worker : workers_) {
        try {
            worker->join();
        } catch (...) {
            if (!error)
                error = std::current_exception();
        }
    }
    if (error)
        std::rethrow_exception(error);
}
} // namespace hp::net
