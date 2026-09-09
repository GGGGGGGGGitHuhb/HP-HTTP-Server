#define HP_MULTI_REACTOR_ENTRY legacy_multi_capacity_entry
#include "multi_reactor_tests.cpp"
#undef HP_MULTI_REACTOR_ENTRY

namespace hp::net {
struct ResourceLimitsTestAccess {
    static std::size_t count(EventLoop& loop) {
        std::lock_guard lock(loop.mutex_);
        return loop.outstanding_;
    }
};

struct ConnectionIoTestAccess {
    static std::size_t size(const ConnectionIo& io) {
        return io.output_.size();
    }

    static std::size_t capacity(const ConnectionIo& io) {
        return io.output_.capacity();
    }
};
} // namespace hp::net

namespace {
void output_bounds() {
    constexpr auto limit = ConnectionIo::output_limit;
    require(ConnectionIo::output_fits(limit - 1, 1) && !ConnectionIo::output_fits(limit, 1) &&
                !ConnectionIo::output_fits(1, SIZE_MAX),
            "overflow safe capacity arithmetic");
    std::vector<std::byte> bytes(limit + 1);
    for (auto size : {limit - 1, limit, limit + 1}) {
        ConnectionIo io{Socket{}};
        bool rejected = false;
        try {
            io.queue_output(std::span(bytes).first(size));
        } catch (const std::length_error&) {
            rejected = true;
        }
        require(rejected == (size > limit), "actual output boundary");
        require(io.pending_bytes() == (rejected ? 0 : size), "whole append or no append");
        if (!rejected) {
            try {
                io.queue_output(std::span(bytes).first(2));
                throw std::runtime_error("overflow append accepted");
            } catch (const std::length_error&) {
            }
            require(io.pending_bytes() == size, "rejected append unchanged");
        }
    }
    int sockets[2];
    require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets) == 0,
            "bounded output socketpair");
    ConnectionIo io{Socket{sockets[0]}};
    Socket peer{sockets[1]};
    int small = 4096;
    ::setsockopt(io.fd(), SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
    std::size_t total = 0, peak_size = 0, peak_capacity = 0, blocked = 0;
    char received[65536];
    for (int cycle = 0; cycle < 1000; ++cycle) {
        io.queue_output(std::span(bytes).first(16384));
        auto written = io.write_available();
        blocked += written.would_block;
        peak_size = std::max(peak_size, ConnectionIoTestAccess::size(io));
        peak_capacity = std::max(peak_capacity, ConnectionIoTestAccess::capacity(io));
        for (;;) {
            const auto n = ::recv(peer.fd(), received, sizeof(received), 0);
            if (n > 0)
                total += static_cast<std::size_t>(n);
            else
                break;
        }
        while (io.pending_bytes() > 16384) {
            (void)io.write_available();
            const auto n = ::recv(peer.fd(), received, sizeof(received), 0);
            if (n > 0)
                total += static_cast<std::size_t>(n);
        }
    }
    require(blocked > 0 && peak_size <= limit && peak_capacity <= limit * 2, "bounded storage");
    std::cout << "output cycles=1000 sent=" << total << " EAGAIN=" << blocked
              << " peak_size=" << peak_size << " peak_capacity=" << peak_capacity << '\n';
}

void loop_bounds() {
    EventLoop loop;
    std::atomic<int> executed{0};
    std::vector<int> accepted(2048), calls(2048);
    std::vector<std::thread> producers;
    for (int producer = 0; producer < 4; ++producer) {
        producers.emplace_back([&, producer] {
            for (int id = producer * 512; id < (producer + 1) * 512; ++id)
                accepted[id] = loop.queue_in_loop([&, id] {
                    ++calls[id];
                    ++executed;
                });
        });
    }
    for (auto& producer : producers)
        producer.join();
    require(std::count(accepted.begin(), accepted.end(), 1) == 1024, "multi producer capacity");
    loop.poll_once(0);
    require(accepted == calls && executed == 1024, "each accepted ID once");
    require(loop.queue_in_loop([&] {
        ++executed;
    }),
            "capacity restored");
    loop.poll_once(0);
    std::cout << "loop accepted=1024 rejected=1024 executed=" << executed << '\n';
}

void release_and_faults() {
    EventLoop loop;
    int accepted_count = 0, executed_count = 0;
    bool injected = false;
    for (int index = 0; index < 100 && !injected; ++index) {
        timer_allocation_failure = 0;
        try {
            require(loop.queue_in_loop([&] {
                ++executed_count;
            }),
                    "allocation probe admitted");
            ++accepted_count;
        } catch (const std::bad_alloc&) {
            injected = true;
        }
        timer_allocation_failure = -1;
    }
    require(injected &&
                ResourceLimitsTestAccess::count(loop) == static_cast<std::size_t>(accepted_count),
            "enqueue allocation failure returns reservation");
    require(loop.queue_in_loop([&] {
        ++executed_count;
    }),
            "enqueue after allocation failure");
    ++accepted_count;
    loop.poll_once(0);
    require(executed_count == accepted_count && ResourceLimitsTestAccess::count(loop) == 0,
            "all reservations released");
    int released = 0;

    struct Capture {
        EventLoop& loop;
        int& released;

        ~Capture() {
            ++released;
            require(!loop.queue_in_loop([] {
            }),
                    "terminal destructor reentry rejected");
            loop.request_force();
        }
    };

    auto capture = std::shared_ptr<Capture>(new Capture{loop, released});
    loop.queue_in_loop([] {
        throw std::runtime_error("capacity original failure");
    });
    loop.queue_in_loop([capture] {
    });
    capture.reset();
    try {
        loop.poll_once(0);
        throw std::runtime_error("missing capacity failure");
    } catch (const std::runtime_error& error) {
        require(std::string(error.what()) == "capacity original failure", "original task failure");
    }
    require(released == 1 && ResourceLimitsTestAccess::count(loop) == 0,
            "fatal batch count released");
    std::cout << "task_fault accepted=" << accepted_count << " executed=" << executed_count
              << " capture_release=" << released
              << " outstanding=" << ResourceLimitsTestAccess::count(loop) << '\n';
}

void saturated_controls() {
    EventLoop loop;
    int drain = 0, force = 0, ran = 0;
    loop.set_control_callback([&](EventLoop::Control control, EventLoop::Deadline) {
        require(loop.is_in_loop_thread(), "control owner");
        if (control == EventLoop::Control::drain)
            ++drain;
        if (control == EventLoop::Control::force)
            ++force;
    });
    for (int id = 0; id < 1024; ++id)
        require(loop.queue_in_loop([&, id] {
            require(drain == 1, "drain before first ordinary callback");
            if (id == 0)
                loop.request_force();
            else
                require(force == 1, "force at callback boundary");
            ++ran;
        }),
                "control full queue");
    require(!loop.queue_in_loop([] {
    }),
            "ordinary queue full");
    loop.request_drain(hp::timer::TimerQueue::Clock::now() + std::chrono::hours(1));
    loop.poll_once(0);
    require(drain == 1 && force == 1 && ran == 1024, "fixed control bypass and accepted drain");
    std::cout << "saturated_control drain=" << drain << " force=" << force << " tasks=" << ran
              << '\n';
}

void oversized_connection() {
    EventLoop loop;
    ConnectionRegistry registry(loop, 0);
    int first[2], second[2];
    require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, first) == 0, "overflow pair");
    require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, second) == 0, "healthy pair");
    Socket peer1(first[1]), peer2(second[1]);
    {
        std::lock_guard lock(socket_probe_mutex);
        accepted_fds.insert(first[0]);
        accepted_fds.insert(second[0]);
        accepted_sockets += 2;
    }
    std::vector<std::byte> excessive(ConnectionIo::output_limit + 1);
    registry.add(Socket(first[0]), [&](TcpConnection& connection, auto, bool) {
        connection.send(excessive);
    });
    registry.add(Socket(second[0]), {});
    require(::send(peer1.fd(), "x", 1, MSG_NOSIGNAL) == 1, "trigger overflow");
    require(::send(peer2.fd(), "ok", 2, MSG_NOSIGNAL) == 2, "healthy request");
    loop.poll_once(0);
    char bytes[4];
    require(::recv(peer1.fd(), bytes, sizeof(bytes), 0) == 0, "overflow has no error response");
    require(::recv(peer2.fd(), bytes, sizeof(bytes), 0) == 2 && std::string_view(bytes, 2) == "ok",
            "healthy connection unaffected");
    require(!loop.failed(), "oversized output is not worker fatal");
    std::cout << "oversized_connection rejected_bytes=" << excessive.size() << " healthy_bytes=2\n";
}

void batch_and_pool() {
    EventLoopThread thread;
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    std::atomic<int> completed{0};
    thread.start([&](EventLoop& loop) {
        for (int id = 0; id < 1024; ++id)
            require(loop.queue_in_loop([&, id, owner = &loop] {
                if (id == 0) {
                    require(!owner->queue_in_loop([] {
                    }),
                            "executing task counts on self post");
                    entered.set_value();
                    require(gate.wait_for(3s) == std::future_status::ready, "batch gate");
                }
                ++completed;
            }),
                    "ready task accepted");
    });
    require(entered.get_future().wait_for(3s) == std::future_status::ready, "batch entered");
    require(!thread.post([](EventLoop&) {
    }),
            "thread rejects when local batch full");
    release.set_value();
    thread.request_stop();
    thread.join();
    require(completed == 1024, "S1 accepted drain preserved");
    EventLoopThreadPool pool;
    std::promise<void> pool_entered, pool_release;
    auto pool_gate = pool_release.get_future().share();
    pool.start(1, [&](std::size_t, EventLoop& loop) {
        for (int id = 0; id < 1024; ++id)
            require(loop.queue_in_loop([&, id] {
                if (id == 0) {
                    pool_entered.set_value();
                    require(pool_gate.wait_for(3s) == std::future_status::ready, "pool gate");
                }
            }),
                    "fill loop bypassing pool");
    });
    require(pool_entered.get_future().wait_for(3s) == std::future_status::ready, "pool entered");
    require(!pool.post(0,
                       [](EventLoop&) {
                       }),
            "pool rollback after loop rejection");
    require(EventLoopThreadPoolTestAccess::outstanding(pool, 0) == 0,
            "double reservation restored");
    pool_release.set_value();
    pool.request_stop();
    pool.join();
    std::cout << "batch executed=" << completed << " pool_rejected_outstanding=0\n";
}
} // namespace

int main() {
    try {
        output_bounds();
        loop_bounds();
        batch_and_pool();
        release_and_faults();
        saturated_controls();
        oversized_connection();
        require(accepted_sockets == closed_sockets && invalid_closes == 0,
                "observed fd closes once");
        std::cout << "socket_closes accepted=" << accepted_sockets << " closed=" << closed_sockets
                  << " invalid=" << invalid_closes << '\n';
        std::cout << "resource_limits_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
