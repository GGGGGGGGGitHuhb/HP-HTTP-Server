// Reuse the original production fixture and close probes without changing its assertions.
#include <climits>
#define HP_MULTI_REACTOR_ENTRY legacy_multi_reactor_entry
#include "multi_reactor_tests.cpp"
#undef HP_MULTI_REACTOR_ENTRY
#define main legacy_server_entry
#include "../app/main.cpp"
#undef main

namespace hp::net {
struct ConnectionTimeoutTestAccess {
    static ConnectionRegistry& registry(TcpServer& server, std::size_t index) {
        return server.worker_count_ ? *server.registries_.at(index)
                                    : *server.main_registry_;
    }

    static EventLoop& loop(ConnectionRegistry& registry) {
        return registry.loop_;
    }

    static auto& entries(ConnectionRegistry& registry) {
        return registry.connections_;
    }

    static auto deadline(EventLoop& loop) {
        return loop.timers_.next_deadline();
    }

    static auto progress(TcpConnection& connection) {
        return connection.last_progress_;
    }

    static auto wait_since(TcpConnection& connection) {
        return connection.wait_since_;
    }

    static auto timer(TcpConnection& connection) {
        return connection.timeout_id_;
    }

    static void event(TcpConnection& connection, std::uint32_t event) {
        connection.handle_event(event);
    }

    static void expire(ConnectionRegistry& registry, int fd,
                       TcpConnection::Identity identity) {
        registry.expire(fd, identity);
    }

    static bool stopping(TcpServer& server) { return server.stopping_.load(); }

    static auto config(TcpServer& server) { return server.timeouts_; }
};
}  // namespace hp::net

namespace {
using Access = ConnectionTimeoutTestAccess;
using Clock = hp::timer::TimerQueue::Clock;

long long ticks(Clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               time.time_since_epoch())
        .count();
}

struct Snapshot {
    std::size_t connections{}, timers{}, waiting{}, pending{};
    long long progress{}, deadline{}, wait{};
};

Snapshot snapshot(ServerHarness& harness, std::size_t workers,
                  std::size_t index = 0) {
    std::promise<Snapshot> done;
    auto result = done.get_future();
    auto inspect = [&](EventLoop& loop) {
        Snapshot value;
        auto& registry = Access::registry(*harness.server, index);
        value.connections = Access::entries(registry).size();
        value.timers = loop.timer_count();
        for (auto& [fd, connection] : Access::entries(registry)) {
            (void)fd;
            value.pending += connection->pending_bytes();
            value.waiting += Access::wait_since(*connection).has_value();
            value.progress = ticks(Access::progress(*connection));
            if (auto wait = Access::wait_since(*connection))
                value.wait = ticks(*wait);
        }
        if (auto deadline = Access::deadline(loop))
            value.deadline = ticks(*deadline);
        done.set_value(value);
    };
    bool accepted;
    if (workers)
        accepted = TcpServerTestAccess::post(*harness.server, index, inspect);
    else
        accepted = TcpServerTestAccess::main_post(*harness.server, [&] {
            inspect(Access::loop(Access::registry(*harness.server, 0)));
        });
    require(accepted, "snapshot task accepted");
    require(result.wait_for(3s) == std::future_status::ready,
            "snapshot deadline");
    return result.get();
}

void idle_modes(const hp::http::StaticFileService& service) {
    for (std::size_t workers : {0U, 1U, 2U}) {
        ServerHarness harness(service, workers, nullptr, {150ms, 0ms});
        Stream client(harness.port);
        Snapshot state;
        await([&] {
            state = snapshot(harness, workers);
            return state.connections == 1;
        });
        require(state.timers == 1 && state.deadline - state.progress == 150000,
                "adopt starts exactly one idle timer");
        client.eof();
        const auto closed = ticks(Clock::now());
        auto after = snapshot(harness, workers);
        require(after.connections == 0 && after.timers == 0,
                "pure timer immediately reclaimed");
        require(closed >= state.deadline, "idle not early");
        harness.stop();
        std::cout << "idle workers=" << workers
                  << " adopted_us=" << state.progress
                  << " deadline_us=" << state.deadline
                  << " closed_us=" << closed << " timers=" << state.timers
                  << "->" << after.timers << '\n';
    }
}

void waiting_states(const hp::http::StaticFileService& service) {
    for (const auto config :
         {ConnectionTimeouts{0ms, 120ms}, ConnectionTimeouts{200ms, 0ms},
          ConnectionTimeouts{200ms, 120ms}, ConnectionTimeouts{0ms, 0ms}}) {
        ServerHarness harness(service, 2, nullptr, config);
        Stream client(harness.port);
        Snapshot initial;
        await([&] {
            initial = snapshot(harness, 2);
            return initial.connections == 1;
        });
        require(initial.waiting == 0 &&
                    initial.timers == (config.idle.count() ? 1U : 0U),
                "initial connection is not keep-alive waiting");
        client.send(query("/note.txt") + query("/missing"));
        response(client.next(), 200, "hello from S3\n");
        response(client.next(), 404, "404 Not Found\n");
        auto state = snapshot(harness, 2);
        require(state.waiting == 1 && state.pending == 0,
                "pipeline drained before wait");
        require(
            state.timers ==
                (config.idle.count() || config.keep_alive.count() ? 1U : 0U),
            "disabled policies allocate no timers");
        auto expected = config.idle.count()
                            ? state.progress + config.idle.count() * 1000
                            : LLONG_MAX;
        if (config.keep_alive.count())
            expected = std::min(expected,
                                state.wait + config.keep_alive.count() * 1000);
        if (state.timers)
            require(state.deadline == expected, "minimum applicable deadline");
        client.send("GET /note.txt HTTP/1.1\r\nHost:");
        Snapshot partial;
        await([&] {
            partial = snapshot(harness, 2);
            return partial.waiting == 0;
        });
        require(partial.progress > state.progress,
                "new bytes refresh idle and exit waiting");
        require(partial.timers == (config.idle.count() ? 1U : 0U),
                "partial has no keep timer");
        client.send(" localhost\r\n\r\n");
        response(client.next(), 200, "hello from S3\n");
        state = snapshot(harness, 2);
        require(state.waiting == 1, "second response enters wait");
        if (state.timers) {
            client.eof();
            require(ticks(Clock::now()) >= state.deadline,
                    "combined deadline not early");
        } else {
            harness.server->request_stop();
        }
        harness.stop();
        std::cout << "waiting idle_ms=" << config.idle.count()
                  << " keep_ms=" << config.keep_alive.count()
                  << " wait_us=" << state.wait
                  << " deadline_us=" << state.deadline
                  << " partial_timers=" << partial.timers
                  << " pipeline_responses=2\n";
    }
}

struct Pair {
    Socket peer;
    int owned;

    Pair() {
        int sockets[2];
        require(
            ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                         sockets) == 0,
            "socketpair");
        owned = sockets[0];
        peer.reset(sockets[1]);
        std::lock_guard lock(socket_probe_mutex);
        accepted_fds.insert(owned);
        ++accepted_sockets;
    }
};

void owner_progress_and_failures(const hp::http::StaticFileService& service) {
    EventLoop loop;
    ConnectionRegistry registry(loop, hp::http::max_request_bytes,
                                {150ms, 100ms});
    Pair pair;
    registry.add(Socket(pair.owned), hp::app::make_http_factory(service)());
    auto& connection = *Access::entries(registry).at(pair.owned);
    const auto initial = Access::progress(connection);
    const auto id = Access::timer(connection);
    Access::event(connection, EPOLLIN | EPOLLOUT);
    require(Access::progress(connection) == initial &&
                Access::timer(connection) == id,
            "EAGAIN and spurious writable do not refresh");
    connection.set_idle_wait(true);
    auto wait = Access::wait_since(connection);
    connection.set_idle_wait(true);
    require(Access::wait_since(connection) == wait,
            "duplicate wait does not renew");
    connection.set_idle_wait(false);
    const std::string partial = "GET /note.txt HTTP/1.1\r\nHost:";
    require(::send(pair.peer.fd(), partial.data(), partial.size(),
                   MSG_NOSIGNAL) == static_cast<ssize_t>(partial.size()),
            "partial send bytes");
    loop.poll_once(0);
    require(Access::progress(connection) > initial &&
                !Access::wait_since(connection),
            "actual recv refresh");
    const auto before_failure = Access::progress(connection);
    timer_allocation_failure = 0;
    try {
        connection.set_idle_wait(true);
        throw std::runtime_error("renewal failure not raised");
    } catch (const std::bad_alloc&) {
    }
    timer_allocation_failure = -1;
    require(connection.state() == TcpConnection::State::closing &&
                loop.timer_count() == 0,
            "renew failure closes connection and cancels record");
    registry.drain_closed_connections();
    require(Access::entries(registry).empty(), "failed renewal reclaimed");
    require(!loop.cancel_timer(id), "old timer id invalid");
    std::cout << "progress recv_bytes=" << partial.size()
              << " adopted_us=" << ticks(initial)
              << " recv_us=" << ticks(before_failure)
              << " EAGAIN_delta=0 renewal_failure_timers=" << loop.timer_count()
              << '\n';
}

void write_progress(const hp::http::StaticFileService& service) {
    EventLoop loop;
    ConnectionRegistry registry(loop, hp::http::max_request_bytes,
                                {150ms, 100ms});
    Pair pair;
    int small = 4096;
    ::setsockopt(pair.owned, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
    int provider_calls = 0;
    registry.add(
        Socket(pair.owned),
        hp::app::make_http_callback([&](const auto& request, auto policy) {
            ++provider_calls;
            return service.handle_response(request, policy);
        }));
    auto& connection = *Access::entries(registry).at(pair.owned);
    const std::string request = query("/large.bin") + query("/note.txt");
    require(::send(pair.peer.fd(), request.data(), request.size(),
                   MSG_NOSIGNAL) == static_cast<ssize_t>(request.size()),
            "large request");
    loop.poll_once(0);
    require(connection.pending_bytes() > 0 && !Access::wait_since(connection),
            "Writing never waits");
    auto progress = Access::progress(connection);
    const auto queued = connection.pending_bytes();
    Access::event(connection, EPOLLOUT);
    require(Access::progress(connection) == progress &&
                connection.pending_bytes() == queued,
            "real send EAGAIN does not refresh");
    char bytes[65536];
    std::size_t received = 0;
    const auto began = Clock::now();
    for (int cycle = 0; cycle < 12; ++cycle) {
        auto n = ::recv(pair.peer.fd(), bytes, sizeof(bytes), 0);
        require(n > 0, "drain real response bytes");
        received += static_cast<std::size_t>(n);
        loop.poll_once(0);
        bool paced = false;
        loop.add_timer(Clock::now() + 30ms, [&] { paced = true; });
        while (!paced)
            loop.poll_once(-1);
    }
    require(Clock::now() - began > 150ms,
            "write progress survives original idle deadline");
    require(Access::progress(connection) > progress &&
                connection.pending_bytes() < queued,
            "actual send progress renews idle");
    progress = Access::progress(connection);
    const auto pending = connection.pending_bytes();
    while (!Access::entries(registry).empty())
        loop.poll_once(-1);
    require(ticks(Clock::now()) >= ticks(progress) + 150000,
            "stalled writer deadline");
    require(loop.timer_count() == 0, "stalled writer timer reclaimed");
    require(provider_calls == 1,
            "buffered suffix provider not called after timeout");
    std::string tail;
    for (;;) {
        auto n = ::recv(pair.peer.fd(), bytes, sizeof(bytes), 0);
        if (n <= 0)
            break;
        tail.append(bytes, static_cast<std::size_t>(n));
    }
    require(tail.find("408") == std::string::npos,
            "no synthetic timeout response");
    std::cout << "write received_bytes=" << received
              << " progress_duration_us=" << ticks(Clock::now()) - ticks(began)
              << " initial_pending=" << queued << " stalled_pending=" << pending
              << " last_send_us=" << ticks(progress)
              << " closed_us=" << ticks(Clock::now())
              << " provider_calls=" << provider_calls
              << " timers=" << loop.timer_count() << '\n';
}

void paced_input(const hp::http::StaticFileService& service) {
    EventLoop loop;
    ConnectionRegistry registry(loop, hp::http::max_request_bytes,
                                {150ms, 100ms});
    Pair pair;
    registry.add(Socket(pair.owned), hp::app::make_http_factory(service)());
    const auto began = Clock::now();
    std::size_t sent = 0;
    for (const std::string_view part : {"GET ", "/note.txt ", "HTTP/1.1\r\n",
                                        "Host: localhost\r\n", "\r\n"}) {
        require(::send(pair.peer.fd(), part.data(), part.size(),
                       MSG_NOSIGNAL) == static_cast<ssize_t>(part.size()),
                "paced request send");
        sent += part.size();
        loop.poll_once(0);
        bool paced = false;
        loop.add_timer(Clock::now() + 50ms, [&] { paced = true; });
        while (!paced)
            loop.poll_once(-1);
        require(Access::entries(registry).size() == 1,
                "positive chunks keep connection alive");
    }
    require(Clock::now() - began > 150ms, "input spans original idle cutoff");
    char bytes[1024];
    const auto count = ::recv(pair.peer.fd(), bytes, sizeof(bytes), 0);
    require(count > 0, "paced request received response");
    const std::string response_bytes(bytes, static_cast<std::size_t>(count));
    require(response_bytes.starts_with("HTTP/1.1 200") &&
                response_bytes.ends_with("hello from S3\n"),
            "paced complete response");
    std::cout << "paced_input sent_bytes=" << sent
              << " response_bytes=" << count
              << " elapsed_us=" << ticks(Clock::now()) - ticks(began)
              << " timers=" << loop.timer_count() << '\n';
}

void healthy_during_timeout(const hp::http::StaticFileService& service) {
    ServerHarness harness(service, 2, nullptr, {150ms, 100ms});
    Stream stalled(harness.port);
    stalled.send(query("/large.bin") + query("/note.txt"));
    Snapshot blocked;
    await([&] {
        blocked = snapshot(harness, 2, 0);
        return blocked.pending > 0;
    });
    require(blocked.waiting == 0, "production stalled writer is not waiting");
    Stream healthy(harness.port);
    healthy.send(query("/note.txt", true));
    response(healthy.next(), 200, "hello from S3\n");
    healthy.eof();
    Snapshot after;
    await([&] {
        after = snapshot(harness, 2, 0);
        return after.connections == 0;
    });
    const auto closed = ticks(Clock::now());
    require(after.timers == 0 && closed >= blocked.deadline,
            "production writer expiry reclaimed");
    harness.stop();
    std::cout << "healthy_during_timeout pending_bytes=" << blocked.pending
              << " deadline_us=" << blocked.deadline << " closed_us=" << closed
              << " healthy_responses=1 timers_after=" << after.timers << '\n';
}

void rollback_and_reuse(const hp::http::StaticFileService& service) {
    EventLoop loop;
    ConnectionRegistry registry(loop, hp::http::max_request_bytes,
                                {150ms, 100ms});
    int failures_seen = 0;
    for (int allocation = 0; allocation < 4; ++allocation) {
        Pair pair;
        auto callback = hp::app::make_http_factory(service)();
        fail_after_registration = allocation;
        try {
            registry.add(Socket(pair.owned), std::move(callback));
            throw std::runtime_error(
                "expected post-registration allocation failure");
        } catch (const std::bad_alloc&) {
            ++failures_seen;
        }
        fail_after_registration = -1;
        timer_allocation_failure = -1;
        require(loop.timer_count() == 0 && Access::entries(registry).empty(),
                "adopt rollback");
        require(::fcntl(pair.owned, F_GETFD) == -1 && errno == EBADF,
                "adopt socket closed");
    }
    Pair original;
    registry.add(Socket(original.owned), hp::app::make_http_factory(service)());
    const auto old_identity =
        Access::entries(registry).at(original.owned)->identity();
    const auto old_timer =
        Access::timer(*Access::entries(registry).at(original.owned));
    Access::entries(registry).at(original.owned)->request_close();
    registry.drain_closed_connections();
    Pair replacement;
    require(replacement.owned == original.owned,
            "real fd reused by socketpair");
    registry.add(Socket(replacement.owned),
                 hp::app::make_http_factory(service)());
    auto& current = *Access::entries(registry).at(replacement.owned);
    require(current.identity() != old_identity,
            "connection identity not reused");
    require(!loop.cancel_timer(old_timer), "old timer cannot cancel new one");
    Access::expire(registry, replacement.owned, old_identity);
    require(current.state() == TcpConnection::State::active &&
                loop.timer_count() == 1,
            "old identity cannot close reused fd");
    bool callback_finished = false;
    loop.add_timer(Clock::now(), [&] {
        current.request_close();
        require(Access::entries(registry).size() == 1,
                "not destroyed inside callback");
        callback_finished = true;
    });
    loop.poll_once(0);
    require(callback_finished && Access::entries(registry).empty() &&
                loop.timer_count() == 0,
            "after_dispatch owns destruction");
    // A ready EOF and expired timer share one poll; IO closes first and cancels its timer.
    Pair eof;
    registry.add(Socket(eof.owned), hp::app::make_http_factory(service)());
    loop.reschedule_timer(
        Access::timer(*Access::entries(registry).at(eof.owned)), Clock::now());
    ::shutdown(eof.peer.fd(), SHUT_WR);
    loop.poll_once(0);
    require(Access::entries(registry).empty() && loop.timer_count() == 0,
            "EOF and expiry once");
    std::cout << "rollback post_registration_failures=" << failures_seen
              << " reused_fd=" << replacement.owned
              << " old_id=" << old_identity
              << " callback_finished=" << callback_finished
              << " remaining=" << loop.timer_count() << '\n';
}

void fatal_timer(const hp::http::StaticFileService& service) {
    ServerHarness harness(service, 2, nullptr, {300ms, 200ms});
    Stream first(harness.port), second(harness.port);
    first.send(query("/note.txt"));
    second.send(query("/missing"));
    response(first.next(), 200, "hello from S3\n");
    response(second.next(), 404, "404 Not Found\n");
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    require(TcpServerTestAccess::post(
                *harness.server, 1,
                [&](EventLoop&) {
                    entered.set_value();
                    require(gate.wait_for(3s) == std::future_status::ready,
                            "fatal timer full worker gate");
                }),
            "block other worker");
    require(entered.get_future().wait_for(3s) == std::future_status::ready,
            "other worker entered");
    for (int i = 1; i < 1024; ++i)
        require(
            TcpServerTestAccess::post(*harness.server, 1, [](EventLoop&) {}),
            "fill other worker");
    require(!TcpServerTestAccess::post(*harness.server, 1, [](EventLoop&) {}),
            "capacity bounded");
    std::atomic<int> callbacks{0}, forbidden{0}, captures{0};
    require(TcpServerTestAccess::post(
                *harness.server, 0,
                [&](EventLoop& loop) {
                    struct Capture {
                        EventLoop& loop;
                        std::atomic<int>& released;

                        ~Capture() {
                            ++released;
                            try {
                                loop.add_timer(Clock::now(), [] {});
                                std::terminate();
                            } catch (const std::logic_error&) {
                            }
                        }
                    };
                    loop.add_timer(Clock::now(), [&] {
                        ++callbacks;
                        throw std::runtime_error("fatal timer original");
                    });
                    auto capture =
                        std::shared_ptr<Capture>(new Capture{loop, captures});
                    loop.add_timer(Clock::now() + 1h,
                                   [&, capture] { ++forbidden; });
                }),
            "install real worker timers");
    await([&] { return Access::stopping(*harness.server); });
    release.set_value();
    harness.failed("fatal timer original");
    require(callbacks == 1 && forbidden == 0 && captures == 1,
            "fatal timer terminal cleanup");
    first.eof();
    second.eof();
    std::cout << "fatal_timer full_other_worker=1024 callbacks=" << callbacks
              << " forbidden=" << forbidden << " capture_release=" << captures
              << '\n';
}

void active_lifecycle(const hp::http::StaticFileService& service) {
    const auto fd = resources("/proc/self/fd");
    const auto threads = resources("/proc/self/task");
    std::size_t timers = 0;
    for (int cycle = 0; cycle < 100; ++cycle) {
        ServerHarness harness(service, 2, nullptr, {300ms, 200ms});
        Stream first(harness.port), second(harness.port);
        first.send(query("/note.txt"));
        second.send(query("/missing"));
        response(first.next(), 200, "hello from S3\n");
        response(second.next(), 404, "404 Not Found\n");
        for (std::size_t index = 0; index < 2; ++index) {
            const auto state = snapshot(harness, 2, index);
            require(state.connections == 1 && state.timers == 1,
                    "active timer per worker");
            timers += state.timers;
        }
        harness.stop();
        first.eof();
        second.eof();
    }
    await([&] { return resources("/proc/self/task") == threads; });
    require(resources("/proc/self/fd") == fd, "timer cycles fd baseline");
    std::cout << "active_lifecycle cycles=100 observed_timers=" << timers
              << " fd=" << fd << '/' << resources("/proc/self/fd")
              << " threads=" << threads << '/' << resources("/proc/self/task")
              << '\n';
}

void options_boundaries() {
    auto parse = [](std::vector<std::string> values) {
        std::vector<char*> argv;
        for (auto& value : values)
            argv.push_back(value.data());
        return parse_options(static_cast<int>(argv.size()), argv.data());
    };
    const auto defaults = parse({"server", "--port", "0", "--root", "."});
    require(defaults.shutdown_timeout == 5000ms,
            "actual production shutdown default");
    require(defaults.timeouts.idle == 30000ms &&
                defaults.timeouts.keep_alive == 15000ms,
            "actual production parser defaults");
    TcpServer component(0);
    require(Access::config(component).idle == 0ms &&
                Access::config(component).keep_alive == 0ms,
            "C++ compatibility defaults");
    int rejected = 0, accepted = 0;
    for (const auto option : {"--idle-timeout-ms", "--keep-alive-timeout-ms"}) {
        for (const auto value : {"0", "1", "86400000"}) {
            auto config =
                parse({"server", "--port", "0", "--root", ".", option, value});
            require((std::string_view(option) == "--idle-timeout-ms"
                         ? config.timeouts.idle
                         : config.timeouts.keep_alive)
                            .count() == std::stoll(value),
                    "valid timeout");
            ++accepted;
        }
        for (const auto value :
             {"86400001", "-1", "+1", "", "1x", "184467440737095516160"}) {
            try {
                (void)parse(
                    {"server", "--port", "0", "--root", ".", option, value});
                throw std::runtime_error("invalid timeout accepted");
            } catch (const std::invalid_argument&) {
                ++rejected;
            }
        }
        for (const auto& values :
             {std::vector<std::string>{"server", option},
              std::vector<std::string>{"server", option, "0", option, "0"}}) {
            try {
                (void)parse(values);
                throw std::runtime_error("missing/repeated accepted");
            } catch (const std::invalid_argument&) {
                ++rejected;
            }
        }
    }
    std::cout << "options defaults_ms=" << defaults.timeouts.idle.count() << '/'
              << defaults.timeouts.keep_alive.count()
              << " cpp_ms=0/0 accepted=" << accepted << " rejected=" << rejected
              << '\n';
}
}  // namespace

int main() {
    try {
        Fixture fixture;
        hp::http::StaticFileService service(fixture.root.string());
        watched_root = hp::http::StaticFileServiceTestAccess::root_fd(service);
        hp::app::set_session_observer_for_test(session_event);
        options_boundaries();
        idle_modes(service);
        waiting_states(service);
        protocols(service, {300ms, 150ms});
        owner_progress_and_failures(service);
        write_progress(service);
        paced_input(service);
        healthy_during_timeout(service);
        rollback_and_reuse(service);
        fatal_timer(service);
        active_lifecycle(service);
        hp::app::set_session_observer_for_test(nullptr);
        require(root_errors == 0 && live_sessions == 0,
                "service outlives timer callbacks");
        require(accepted_sockets == closed_sockets && invalid_closes == 0,
                "exact socket closes");
        std::cout << "close accepted=" << accepted_sockets
                  << " closed=" << closed_sockets
                  << " invalid=" << invalid_closes
                  << " session_events=" << root_checks << '\n';
        std::cout << "connection_timeout_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
