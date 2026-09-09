// Reuse only the established fixture/client-process utilities and old assertions.
#define main legacy_http_integration_entry
#include "http_server_integration_tests.cpp"
#undef main
#include "http_connection_handler.h"
#include "net/tcp_server.h"
#include <atomic>
#include <barrier>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <optional>
#include <thread>

using namespace hp::net;
using namespace std::chrono_literals;

namespace hp::http {
struct StaticFileServiceTestAccess {
    static int root_fd(const StaticFileService& service) {
        return service.root_fd_;
    }
};
} // namespace hp::http

namespace hp::app {
void set_session_observer_for_test(void (*observer)(bool, const void*) noexcept);
}

namespace hp::net {
struct EventLoopThreadPoolTestAccess {
    static std::size_t outstanding(EventLoopThreadPool& pool, std::size_t index) {
        std::lock_guard lock(pool.mutex_);
        return pool.outstanding_.at(index);
    }
};

struct TcpServerTestAccess {
    static bool post(TcpServer& server, std::size_t index, EventLoopThread::Callback task) {
        return server.pool_.post(index, std::move(task));
    }

    static std::size_t outstanding(TcpServer& server, std::size_t index) {
        return EventLoopThreadPoolTestAccess::outstanding(server.pool_, index);
    }

    static bool main_post(TcpServer& server, EventLoop::Task task) {
        return server.loop_.queue_in_loop(std::move(task));
    }
};
} // namespace hp::net

namespace {
thread_local int timer_allocation_failure = -1;
thread_local int fail_after_registration = -1;
std::mutex socket_probe_mutex;
std::set<int> accepted_fds;
std::atomic<int> accepted_sockets{}, closed_sockets{}, invalid_closes{}, reject_registration{},
    reject_allocation{};
} // namespace

extern "C" {
int __real_accept4(int, sockaddr*, socklen_t*, int);

int __wrap_accept4(int fd, sockaddr* address, socklen_t* size, int flags) {
    const int accepted = __real_accept4(fd, address, size, flags);
    if (accepted >= 0) {
        std::lock_guard lock(socket_probe_mutex);
        accepted_fds.insert(accepted);
        ++accepted_sockets;
    }
    return accepted;
}

int __real_close(int);

int __wrap_close(int fd) {
    {
        std::lock_guard lock(socket_probe_mutex);
        if (accepted_fds.erase(fd))
            ++closed_sockets;
    }
    const int result = __real_close(fd);
    if (result < 0 && errno == EBADF)
        ++invalid_closes;
    return result;
}

int __real_epoll_ctl(int, int, int, epoll_event*);

int __wrap_epoll_ctl(int fd, int operation, int observed_fd, epoll_event* event) {
    if (operation == EPOLL_CTL_ADD && reject_registration.load()) {
        std::lock_guard lock(socket_probe_mutex);
        if (accepted_fds.contains(observed_fd) && reject_registration.exchange(0)) {
            errno = EIO;
            return -1;
        }
    }
    const int result = __real_epoll_ctl(fd, operation, observed_fd, event);
    if (result == 0 && operation == EPOLL_CTL_ADD && fail_after_registration >= 0) {
        timer_allocation_failure = fail_after_registration;
        fail_after_registration = -1;
    }
    return result;
}

void* __real__Znwm(std::size_t);

void* __wrap__Znwm(std::size_t size) {
    if (timer_allocation_failure == 0) {
        timer_allocation_failure = -1;
        throw std::bad_alloc();
    }
    if (timer_allocation_failure > 0)
        --timer_allocation_failure;
    if (size == sizeof(TcpConnection) && reject_allocation.exchange(0))
        throw std::bad_alloc();
    return __real__Znwm(size);
}
}

namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

template <class F> void await(F predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < deadline, "deadline");
        std::this_thread::yield();
    }
}

std::size_t resources(const char* path) {
    return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator(path), {}));
}

struct Probe {
    std::mutex mutex;
    std::thread::id main;
    std::vector<std::thread::id> factories, owners;
    std::map<const void*, std::thread::id> sessions;
    int created{}, destroyed{}, owner_errors{}, callback_releases{};
};

std::atomic<Probe*> observed{nullptr};
std::atomic<int> watched_root{-1}, root_checks{}, root_errors{}, live_sessions{};

void session_event(bool created, const void* session) noexcept {
    ++root_checks;
    if (::fcntl(watched_root.load(), F_GETFD) < 0)
        ++root_errors;
    if (created)
        ++live_sessions;
    else
        --live_sessions;
    if (auto* probe = observed.load()) {
        std::lock_guard lock(probe->mutex);
        if (created) {
            if (std::find(probe->owners.begin(), probe->owners.end(), std::this_thread::get_id()) ==
                probe->owners.end())
                ++probe->owner_errors;
            probe->sessions.emplace(session, std::this_thread::get_id());
            ++probe->created;
        } else {
            if (probe->sessions.at(session) != std::this_thread::get_id())
                ++probe->owner_errors;
            probe->sessions.erase(session);
            ++probe->destroyed;
        }
    }
}

struct ServerHarness {
    std::thread control;
    TcpServer* server{};
    std::uint16_t port{};
    std::exception_ptr error;

    ServerHarness(const hp::http::StaticFileService& service, std::size_t workers,
                  Probe* probe = nullptr, ConnectionTimeouts timeouts = {}) {
        std::promise<void> ready;
        auto result = ready.get_future();
        control = std::thread([&, workers, probe, timeouts] {
            bool published = false;
            try {
                auto production = hp::app::make_http_factory(service);
                TcpServer instance(
                    0,
                    [&, production]() mutable {
                        auto callback = production();
                        if (!probe)
                            return callback;
                        std::size_t id;
                        {
                            std::lock_guard lock(probe->mutex);
                            probe->factories.push_back(std::this_thread::get_id());
                            id = probe->owners.size();
                            probe->owners.push_back({});
                        }
                        return TcpConnection::MessageCallback(
                            [callback = std::move(callback), probe,
                             id](TcpConnection& connection, std::span<const std::byte> bytes,
                                 bool eof) mutable {
                                {
                                    std::lock_guard lock(probe->mutex);
                                    auto& owner = probe->owners[id];
                                    if (owner != std::thread::id{} &&
                                        owner != std::this_thread::get_id())
                                        ++probe->owner_errors;
                                    owner = std::this_thread::get_id();
                                }
                                callback(connection, bytes, eof);
                            });
                    },
                    hp::http::max_request_bytes, workers, timeouts);
                if (probe)
                    probe->main = std::this_thread::get_id();
                server = &instance;
                port = instance.bound_port();
                published = true;
                ready.set_value();
                instance.run();
            } catch (...) {
                error = std::current_exception();
                if (!published)
                    ready.set_exception(error);
            }
        });
        try {
            require(result.wait_for(3s) == std::future_status::ready, "server ready deadline");
            result.get();
        } catch (...) {
            control.join();
            throw;
        }
    }

    ~ServerHarness() {
        if (control.joinable()) {
            server->request_stop();
            control.join();
        }
    }

    void stop() {
        server->request_stop();
        control.join();
        if (error)
            std::rethrow_exception(error);
    }

    void failed(const char* message) {
        control.join();
        require(error != nullptr, "server must report failure");
        try {
            std::rethrow_exception(error);
        } catch (const std::runtime_error& caught) {
            require(std::string(caught.what()) == message, "first fatal exception preserved");
        }
    }
};

struct WireResponse {
    int status;
    std::string header, body;
};

struct Stream {
    int fd;
    std::string pending;

    explicit Stream(std::uint16_t port) : fd(connect_client(port)) {}

    ~Stream() {
        ::close(fd);
    }

    void send(std::string_view data) {
        send_all(fd, data);
    }

    WireResponse next() {
        auto read = [&] {
            char bytes[8192];
            auto n = ::recv(fd, bytes, sizeof(bytes), 0);
            require(n > 0, "response recv");
            pending.append(bytes, static_cast<std::size_t>(n));
        };
        while (pending.find("\r\n\r\n") == std::string::npos)
            read();
        const auto boundary = pending.find("\r\n\r\n") + 4;
        auto header = pending.substr(0, boundary);
        const auto at = header.find("Content-Length: ");
        require(at != std::string::npos, "content length");
        const auto length = std::stoull(header.substr(at + 16));
        while (pending.size() < boundary + length)
            read();
        auto body = pending.substr(boundary, length);
        pending.erase(0, boundary + length);
        return {std::stoi(header.substr(9, 3)), std::move(header), std::move(body)};
    }

    void eof() {
        char byte;
        require(pending.empty() && ::recv(fd, &byte, 1, 0) == 0, "terminal EOF");
    }
};

std::string query(std::string_view path, bool close = false) {
    return "GET " + std::string(path) + " HTTP/1.1\r\nHost: localhost\r\n" +
           (close ? "Connection: close\r\n" : "") + "\r\n";
}

void response(const WireResponse& value, int status, std::string_view body) {
    require(value.status == status && value.body == body, "exact response status/body");
}

void owners(const hp::http::StaticFileService& service, std::size_t workers) {
    Probe probe;
    observed = &probe;
    ServerHarness harness(service, workers, &probe);
    for (int i = 0; i < 6; ++i) {
        Stream client(harness.port);
        client.send(query("/note.txt", true));
        response(client.next(), 200, "hello from S3\n");
        client.eof();
    }
    harness.stop();
    observed = nullptr;
    require(probe.factories.size() == 6 && probe.created == 6 && probe.destroyed == 6,
            "session accounting");
    for (int i = 0; i < 6; ++i) {
        require(probe.factories[i] == probe.main, "factory main owner");
        require(probe.owners[i] == probe.owners[i % (workers ? workers : 1)], "fixed round robin");
        require((probe.owners[i] == probe.main) == (workers == 0), "worker mode owner");
    }
    if (workers == 2)
        require(probe.owners[0] != probe.owners[1], "two workers have real IO");
    require(probe.owner_errors == 0 && probe.sessions.empty(), "session lifetime owner");
    std::cout << "owners: workers=" << workers << " accepted=" << probe.factories.size()
              << " session_created=" << probe.created << " destroyed=" << probe.destroyed
              << " route=";
    for (auto owner : probe.owners)
        std::cout << (owner == probe.owners[0] ? '0' : '1');
    std::cout << '\n';
}

void protocols(const hp::http::StaticFileService& service, ConnectionTimeouts timeouts = {}) {
    ServerHarness harness(service, 2, nullptr, timeouts);
    std::barrier begin(3);
    std::exception_ptr errors[2];
    std::thread clients[2];
    for (int i = 0; i < 2; ++i) {
        clients[i] = std::thread([&, i] {
            try {
                Stream client(harness.port);
                begin.arrive_and_wait();
                auto first = query(i ? "/" : "/note.txt");
                client.send(first.substr(0, first.size() - 1));
                client.send(first.substr(first.size() - 1) + query("/missing") +
                            query("/note.txt"));
                response(client.next(), 200,
                         i ? "<h1>integration index</h1>\n" : "hello from S3\n");
                response(client.next(), 404, "404 Not Found\n");
                response(client.next(), 200, "hello from S3\n");
                client.send("GET / HTTP/1.1\r\nBad\r\n\r\n");
                response(client.next(), 400, "400 Bad Request\n");
                client.eof();
            } catch (...) {
                errors[i] = std::current_exception();
            }
        });
    }
    begin.arrive_and_wait();
    for (auto& client : clients)
        client.join();
    for (auto error : errors)
        if (error)
            std::rethrow_exception(error);
    {
        Stream fin(harness.port);
        fin.send(query("/note.txt") + query("/note.txt"));
        ::shutdown(fin.fd, SHUT_WR);
        response(fin.next(), 200, "hello from S3\n");
        response(fin.next(), 200, "hello from S3\n");
        fin.eof();
    }
    {
        Stream reset(harness.port);
        reset.send("GET / HTTP/1.1\r\nHost:");
        linger immediate{1, 0};
        ::setsockopt(reset.fd, SOL_SOCKET, SO_LINGER, &immediate, sizeof(immediate));
    }
    for (auto path : {"/escape.txt", "/../sibling-secret.txt"}) {
        Stream client(harness.port);
        client.send(query(path, true));
        response(client.next(), 403, "403 Forbidden\n");
        client.eof();
    }
    Stream survivor(harness.port);
    survivor.send(query("/note.txt", true));
    response(survivor.next(), 200, "hello from S3\n");
    survivor.eof();
    harness.stop();
    std::cout << "protocols: concurrent_pipeline=2 per_pipeline_responses=4 fin_responses=2 "
                 "rst_survivor=1 security=2\n";
}

void lifecycle(const hp::http::StaticFileService& service) {
    auto fd = resources("/proc/self/fd");
    auto threads = resources("/proc/self/task");
    for (int i = 0; i < 100; ++i) {
        ServerHarness harness(service, 2);
        Stream client(harness.port);
        client.send(query("/note.txt"));
        response(client.next(), 200, "hello from S3\n");
        harness.stop();
        client.eof();
    }
    await([&] {
        return resources("/proc/self/task") == threads;
    });
    require(resources("/proc/self/fd") == fd, "100 cycle fd baseline");
    std::cout << "lifecycle: cycles=100 fd=" << fd << '/' << resources("/proc/self/fd")
              << " threads=" << threads << '/' << resources("/proc/self/task") << '\n';
}

void failures_and_pending(const hp::http::StaticFileService& service) {
    {
        ServerHarness harness(service, 2);
        std::promise<void> entered, release;
        auto gate = release.get_future().share();
        require(TcpServerTestAccess::post(
                    *harness.server, 0,
                    [&](EventLoop&) {
                        entered.set_value();
                        require(gate.wait_for(3s) == std::future_status::ready, "fatal gate");
                        throw std::runtime_error("fatal worker original");
                    }),
                "fatal task accepted");
        require(entered.get_future().wait_for(3s) == std::future_status::ready,
                "fatal worker entered");
        for (int i = 1; i < 1024; ++i)
            require(TcpServerTestAccess::post(*harness.server, 0,
                                              [](EventLoop&) {
                                              }),
                    "fill worker queue");
        require(!TcpServerTestAccess::post(*harness.server, 0,
                                           [](EventLoop&) {
                                           }),
                "full before failure");
        release.set_value();
        harness.failed("fatal worker original");
    }
    {
        ServerHarness harness(service, 2);
        TcpServerTestAccess::main_post(*harness.server, [] {
            throw std::runtime_error("fatal main original");
        });
        harness.failed("fatal main original");
    }
    {
        ServerHarness harness(service, 2);
        Stream active(harness.port);
        active.send(query("/note.txt"));
        response(active.next(), 200, "hello from S3\n");
        std::promise<void> entered, release;
        auto gate = release.get_future().share();
        TcpServerTestAccess::post(*harness.server, 1, [&](EventLoop&) {
            entered.set_value();
            require(gate.wait_for(3s) == std::future_status::ready, "pending gate");
        });
        require(entered.get_future().wait_for(3s) == std::future_status::ready,
                "pending worker blocked");
        Stream pending(harness.port);
        await([&] {
            return TcpServerTestAccess::outstanding(*harness.server, 1) == 2;
        });
        harness.server->request_stop();
        release.set_value();
        harness.stop();
        active.eof();
        pending.eof();
    }
    std::cout << "failure: full_worker_and_main_originals=2 pending_stop=1 all_joined\n";
}

void adoption_failures(const hp::http::StaticFileService& service) {
    auto accepted = accepted_sockets.load();
    auto closed = closed_sockets.load();
    {
        ServerHarness harness(service, 2);
        reject_registration = 1;
        Stream rejected(harness.port);
        rejected.eof();
        Stream survivor(harness.port);
        survivor.send(query("/note.txt", true));
        response(survivor.next(), 200, "hello from S3\n");
        survivor.eof();
        harness.stop();
    }
    require(accepted_sockets - accepted == 2 && closed_sockets - closed == 2,
            "registration failure closes once and peer survives");
    accepted = accepted_sockets.load();
    closed = closed_sockets.load();
    {
        ServerHarness harness(service, 2);
        reject_allocation = 1;
        Stream rejected(harness.port);
        rejected.eof();
        harness.control.join();
        require(harness.error != nullptr, "allocation failure propagated");
        bool bad_alloc = false;
        try {
            std::rethrow_exception(harness.error);
        } catch (const std::bad_alloc&) {
            bad_alloc = true;
        }
        require(bad_alloc, "allocation original type");
    }
    require(accepted_sockets - accepted == 1 && closed_sockets - closed == 1,
            "allocation closes exactly once");
    accepted = accepted_sockets.load();
    closed = closed_sockets.load();
    {
        ServerHarness harness(service, 1);
        std::promise<void> entered, release;
        auto gate = release.get_future().share();
        TcpServerTestAccess::post(*harness.server, 0, [&](EventLoop&) {
            entered.set_value();
            require(gate.wait_for(3s) == std::future_status::ready, "reject gate");
        });
        require(entered.get_future().wait_for(3s) == std::future_status::ready, "reject entered");
        for (int i = 1; i < 1024; ++i)
            require(TcpServerTestAccess::post(*harness.server, 0,
                                              [](EventLoop&) {
                                              }),
                    "reject fill");
        Stream rejected(harness.port);
        rejected.eof();
        release.set_value();
        harness.stop();
    }
    require(accepted_sockets - accepted == 1 && closed_sockets - closed == 1,
            "handoff rejection closes exactly once");
    std::cout << "adoption: register_failure=1 allocation_failure=1 capacity_rejection=1 "
                 "exact_close=verified\n";
}

void cli_modes(const char* executable, const Fixture& fixture) {
    const char* configured = std::getenv("HP_HTTP_TEST_THREADS");
    const auto previous = configured ? std::optional<std::string>(configured) : std::nullopt;
    for (int count : {-1, 0, 1, 2}) {
        if (count < 0)
            ::unsetenv("HP_HTTP_TEST_THREADS");
        else
            ::setenv("HP_HTTP_TEST_THREADS", std::to_string(count).c_str(), 1);
        auto server = start_server(executable, fixture.root);
        const auto expected = static_cast<std::size_t>((count < 0 ? 2 : count) + 1);
        require(server.thread_count() == expected, "production CLI OS thread count");
        Stream client(server.port());
        client.send(query("/note.txt", true));
        response(client.next(), 200, "hello from S3\n");
        client.eof();
        std::cout << "CLI: threads_option=" << count << " OS_threads=" << server.thread_count()
                  << '\n';
    }
    if (previous)
        ::setenv("HP_HTTP_TEST_THREADS", previous->c_str(), 1);
    else
        ::unsetenv("HP_HTTP_TEST_THREADS");
}

} // namespace

#ifndef HP_MULTI_REACTOR_ENTRY
#define HP_MULTI_REACTOR_ENTRY main
#endif
int HP_MULTI_REACTOR_ENTRY(int argc, char** argv) {
    try {
        Fixture fixture;
        auto service_owner = std::make_unique<hp::http::StaticFileService>(fixture.root.string());
        auto& service = *service_owner;
        watched_root = hp::http::StaticFileServiceTestAccess::root_fd(service);
        hp::app::set_session_observer_for_test(session_event);
        owners(service, 0);
        owners(service, 1);
        owners(service, 2);
        protocols(service);
        failures_and_pending(service);
        adoption_failures(service);
        lifecycle(service);
        if (argc == 2)
            cli_modes(argv[1], fixture);
        hp::app::set_session_observer_for_test(nullptr);
        require(live_sessions == 0 && root_checks > 0 && root_errors == 0,
                "service outlives all sessions");
        service_owner.reset();
        require(::fcntl(watched_root, F_GETFD) == -1 && errno == EBADF,
                "service root closed after join");
        std::cout << "service_lifetime: session_events=" << root_checks
                  << " premature_close=" << root_errors << " live_sessions=" << live_sessions
                  << " root_closed_after_join=1\n";
        require(invalid_closes == 0 && accepted_sockets == closed_sockets,
                "accepted socket close accounting");
        std::cout << "socket_ownership: accepted=" << accepted_sockets
                  << " closed=" << closed_sockets << " invalid_closes=" << invalid_closes << '\n';
        std::cout << "multi_reactor_tests: PASS\n";
        return failures ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
