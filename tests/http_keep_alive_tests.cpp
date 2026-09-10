#include "http_connection_handler.h"
#include "http_protocol_boundary_cases.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <memory>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace hp::net {
struct ConnectionIoTestAccess {
    static auto capacity(const ConnectionIo& io) {
        return io.output_.capacity();
    }
};

struct TcpConnectionTestAccess {
    static auto capacity(const TcpConnection& c) {
        return ConnectionIoTestAccess::capacity(c.io_);
    }

    static auto result(TcpConnection& c) { return c.last_result_; }

    static auto events(TcpConnection& c) { return c.event_count_; }

    static auto interest(TcpConnection& c) { return c.channel_.interest(); }
};
}

namespace {
using namespace hp;
using namespace hp::net;
using C = TcpConnectionTestAccess;
int failures{};

void expect(bool ok, const char* why) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << why << '\n';
    }
}

std::span<const std::byte> bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

std::string text(const std::vector<std::byte>& v) {
    return {reinterpret_cast<const char*>(v.data()), v.size()};
}

std::string request(std::string_view target = "/a",
                    std::string_view header = "") {
    return "GET " + std::string(target) + " HTTP/1.1\r\nHost: a\r\n" +
           std::string(header) + "\r\n";
}

std::string response(
    std::string_view body,
    http::ConnectionPolicy policy = http::ConnectionPolicy::keep_alive) {
    return text(http::make_response(http::Status::ok, bytes(body), "text/plain",
                                    false, policy));
}

struct Pair {
    Socket owner, peer;

    Pair() {
        int f[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                         f))
            throw std::runtime_error("socketpair");
        owner.reset(f[0]);
        peer.reset(f[1]);
    }

    void send(std::string_view s) {
        expect(::send(peer.fd(), s.data(), s.size(), MSG_NOSIGNAL) ==
                   static_cast<ssize_t>(s.size()),
               "send all controlled request bytes");
    }

    std::string collect() {
        std::string out;
        char b[32768];
        ssize_t n;
        while ((n = ::recv(peer.fd(), b, sizeof(b), MSG_DONTWAIT)) > 0)
            out.append(b, static_cast<std::size_t>(n));
        return out;
    }
};

std::string pump(EventLoop& loop, Pair& pair, TcpConnection& c,
                 std::size_t expected) {
    std::string out;
    for (int i = 0; i < 10000 && out.size() < expected; ++i) {
        loop.poll_once(0);
        out += pair.collect();
    }
    expect(out.size() == expected,
           "bounded pump received exact response bytes");
    if (c.state() == TcpConnection::State::active)
        expect(!(C::interest(c) & EPOLLOUT), "EPOLLOUT removed after drain");
    return out;
}

void pipeline_slow_and_repeated() {
    EventLoop loop;
    Pair p;
    int size = 4096;
    ::setsockopt(p.owner.fd(), SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    const std::string big(512 * 1024, 'q');
    std::size_t calls{}, closes{}, eagain{};
    TcpConnection c(
        loop, std::move(p.owner), 1,
        app::make_http_callback([&](const http::HttpRequest& r,
                                    http::ConnectionPolicy policy) {
            ++calls;
            return http::ResponseResult{
                http::make_response(http::Status::ok,
                                    bytes(r.target == "/big" ? big : r.target),
                                    "text/plain", false, policy),
                policy};
        }),
        http::max_request_bytes, [&](int, auto) { ++closes; });
    c.start();
    p.send(request("/big") + request("/b") + request("/c"));
    loop.poll_once(100);
    expect(
        calls == 1 && C::result(c).write_would_block && c.pending_bytes() > 0,
        "real EAGAIN holds only first response");
    const auto pending = c.pending_bytes(), buffered = c.input_view().size(),
               events = C::events(c);
    eagain += C::result(c).write_would_block;
    expect(buffered == request("/b").size() + request("/c").size() &&
               buffered <= http::max_request_bytes,
           "bounded suffix stays in transport");
    p.send(request("/later"));
    for (int i = 0; i < 10; ++i)
        loop.poll_once(0);
    expect(calls == 1 && c.input_view().size() == buffered &&
               C::events(c) == events,
           "pause has no recv or busy event while peer writes");
    const auto wanted =
        response(big) + response("/b") + response("/c") + response("/later");
    auto got = pump(loop, p, c, wanted.size());
    expect(
        got == wanted && calls == 4 && closes == 0,
        "cached pipeline and queued kernel data progress in order without new send");
    std::size_t max_pending = pending;
    const auto capacity = C::capacity(c);
    for (int n = 0; n < 24; ++n) {
        p.send(request(n % 2 ? "/b" : "/big"));
        loop.poll_once(100);
        eagain += C::result(c).write_would_block;
        max_pending = std::max(max_pending, c.pending_bytes());
        auto want = response(n % 2 ? "/b" : big);
        expect(pump(loop, p, c, want.size()) == want,
               "repeated small/large requests preserve bytes");
    }
    expect(C::capacity(c) == capacity,
           "output capacity does not grow over repeated requests");
    expect(max_pending <= response(big).size() && calls == 28,
           "pending bound independent of request count");
    std::string many, expected_many;
    for (int i = 0; i < 600; ++i) {
        many += request("/b");
        expected_many += response("/b");
    }
    expect(many.size() > http::max_request_bytes,
           "cumulative connection input exceeds per-request limit");
    p.send(many);
    expect(
        pump(loop, p, c, expected_many.size()) == expected_many && calls == 628,
        "600 short pipelined requests reset independent limits");
    expect(C::capacity(c) == capacity,
           "pipeline depth does not grow output capacity");
    const auto idle = C::events(c);
    for (int i = 0; i < 20; ++i)
        loop.poll_once(0);
    expect(idle == C::events(c), "keep-alive idle no self excitation");
    ::shutdown(p.peer.fd(), SHUT_WR);
    loop.poll_once(100);
    expect(closes == 1 && p.collect().empty(),
           "idle EOF after requests closes silently once");
    std::cout << "slow: EAGAIN=" << eagain
              << " blocked_provider=1 suffix=" << buffered
              << " initial_pending=" << pending
              << " max_pending=" << max_pending
              << " cumulative_short_bytes=" << many.size()
              << " output_capacity=" << capacity << " requests=" << calls
              << " idle_polls=20 close=" << closes << '\n';
}

void terminal_matrix_and_eof() {
    const std::vector<std::string> invalid = {
        "Content-Length: 1\r\n",
        "Content-Length: 0\r\nContent-Length: 0\r\n",
        "Content-Length: 0,0\r\n",
        "Content-Length: -0\r\n",
        "Content-Length: +0\r\n",
        "Content-Length: \r\n",
        "Content-Length: 0 0\r\n",
        "Content-Length: 9999999999999999999999999\r\n",
        "Transfer-Encoding: chunked\r\n",
        "Transfer-Encoding: \r\n",
        "Transfer-Encoding: x\r\nTransfer-Encoding: y\r\n",
        "Transfer-Encoding: chunked\r\nContent-Length: 0\r\n",
        "Content-Length: 0\r\nTransfer-Encoding: chunked\r\n",
        "Expect: 100-continue\r\n",
        "Expect: \r\n",
        "Connection: bad token\r\n",
        "Connection: @\r\n",
        "Connection: content-length\r\nContent-Length: 1\r\n"};
    std::size_t cases{};
    auto run = [&](const std::string& input, const std::string& wanted,
                   std::size_t expected_calls, bool eof) {
        EventLoop loop;
        Pair p;
        std::size_t calls{}, closes{};
        TcpConnection c(
            loop, std::move(p.owner), 2,
            app::make_http_callback(
                [&](const http::HttpRequest& r, http::ConnectionPolicy policy) {
                    ++calls;
                    if (r.target == "/throw")
                        throw std::runtime_error("provider");
                    if (r.target == "/403" || r.target == "/404" ||
                        r.target == "/500")
                        return http::ResponseResult{
                            http::make_error_response(
                                r.target == "/403" ? http::Status::forbidden
                                : r.target == "/404"
                                    ? http::Status::not_found
                                    : http::Status::internal_server_error,
                                policy),
                            policy};
                    return http::ResponseResult{
                        http::make_response(http::Status::ok, bytes(r.target),
                                            "text/plain", false, policy),
                        policy};
                }),
            http::max_request_bytes, [&](int, auto) { ++closes; });
        c.start();
        if (!input.empty())
            p.send(input);
        if (eof)
            ::shutdown(p.peer.fd(), SHUT_WR);
        const auto got = pump(loop, p, c, wanted.size());
        // EOF may require a subsequent event after the final successful response.
        for (int i = 0; i < 5; ++i)
            loop.poll_once(0);
        expect(got == wanted && p.collect().empty() &&
                   calls == expected_calls && closes == 1,
               "terminal/EOF exact ordered stream and no suffix provider");
        ++cases;
    };
    const auto bad = text(http::make_error_response(http::Status::bad_request));
    for (const auto& h : invalid)
        run(request("/a") + request("/bad", h) + request("/suffix"),
            response("/a") + bad, 1, false);
    for (const auto& h :
         {"Connection: ClOsE,keep-alive\r\nConnection: keep-alive\r\n",
          "Connection: keep-alive\r\nConnection: x, close\r\n"}) {
        run(request("/a", h) + request("/suffix"),
            response("/a", http::ConnectionPolicy::close), 1, false);
        run(request("/a") + request("/b", h) + request("/suffix"),
            response("/a") + response("/b", http::ConnectionPolicy::close), 2,
            false);
    }
    run(request("/a") + "POST / HTTP/1.1\r\nHost: a\r\n\r\n" +
            request("/suffix"),
        response("/a") +
            text(http::make_error_response(http::Status::method_not_allowed)),
        1, false);
    run(request("/a") + request("/throw") + request("/suffix"),
        response("/a") + text(http::make_error_response(
                             http::Status::internal_server_error)),
        2, false);
    run("", bad, 0, true);
    run(request("/a") + "GET /b HTTP/1.1\r\nHost:", response("/a") + bad, 1,
        true);
    run(request("/a") + request("/b") + request("/c"),
        response("/a") + response("/b") + response("/c"), 3, true);
    for (const auto& target : {"/403", "/404", "/500"}) {
        auto status = std::string_view(target) == "/403"
                          ? http::Status::forbidden
                      : std::string_view(target) == "/404"
                          ? http::Status::not_found
                          : http::Status::internal_server_error;
        run(request(target) + request("/b", "Connection: close\r\n"),
            text(http::make_error_response(
                status, http::ConnectionPolicy::keep_alive)) +
                response("/b", http::ConnectionPolicy::close),
            2, false);
    }
    std::cout << "terminal/eof: cases=" << cases
              << " rejected_framing=" << invalid.size()
              << " suffix_calls=0 duplicate_responses=0\n";
}

void service_policy_and_fin() {
    const auto base =
        std::filesystem::path(std::getenv("HP_S3_TEST_TMP_ROOT")
                                  ? std::getenv("HP_S3_TEST_TMP_ROOT")
                                  : ".cache/olympus-v0.3-s2/builder/tmp");
    const auto root = base / ("keep-service-" + std::to_string(::getpid()));
    std::filesystem::create_directories(root);

    struct Cleanup {
        std::filesystem::path path;

        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{root};

    std::ofstream(root / "index.html") << "index";
    http::StaticFileService service(root.string());
    for (bool prefix : {false, true}) {
        EventLoop loop;
        Pair p;
        std::size_t calls{}, closed{};
        TcpConnection c(
            loop, std::move(p.owner), 4,
            app::make_http_callback(
                [&](const http::HttpRequest& r, http::ConnectionPolicy policy) {
                    ++calls;
                    return service.handle_response(r, policy);
                }),
            http::max_request_bytes, [&](int, auto) { ++closed; });
        c.start();
        p.send((prefix ? request("/") : "") + request("/bad%20target") +
               request("/suffix"));
        auto wanted =
            (prefix ? text(service.handle({"GET", "/"},
                                          http::ConnectionPolicy::keep_alive))
                    : "") +
            text(http::make_error_response(http::Status::bad_request));
        expect(pump(loop, p, c, wanted.size()) == wanted &&
                   calls == (prefix ? 2U : 1U) && closed == 1,
               "real service 400 metadata stops first/second request suffix");
    }
    {
        EventLoop loop;
        Pair p;
        int calls{}, closed{};
        TcpConnection c(
            loop, std::move(p.owner), 5,
            app::make_http_callback(
                [&](const http::HttpRequest&, http::ConnectionPolicy) {
                    ++calls;
                    return http::ResponseResult{
                        http::make_response(http::Status::ok, bytes("INVALID"),
                                            "text/plain", false,
                                            http::ConnectionPolicy::keep_alive),
                        http::ConnectionPolicy::keep_alive};
                }),
            http::max_request_bytes, [&](int, auto) { ++closed; });
        c.start();
        p.send(request("/", "Connection: close\r\n") + request("/suffix"));
        const auto wanted = text(
            http::make_error_response(http::Status::internal_server_error));
        expect(
            pump(loop, p, c, wanted.size()) == wanted && calls == 1 &&
                closed == 1,
            "provider cannot relax explicit close; invalid bytes replaced by500 before send");
    }
    {
        EventLoop loop;
        Pair p;
        int small = 4096;
        ::setsockopt(p.owner.fd(), SOL_SOCKET, SO_SNDBUF, &small,
                     sizeof(small));
        const std::string big(512 * 1024, 'F');
        int calls{}, closed{};
        TcpConnection c(
            loop, std::move(p.owner), 6,
            app::make_http_callback(
                [&](const http::HttpRequest& r, http::ConnectionPolicy policy) {
                    ++calls;
                    return http::ResponseResult{
                        http::make_response(
                            http::Status::ok,
                            bytes(r.target == "/big" ? big : r.target),
                            "text/plain", false, policy),
                        policy};
                }),
            http::max_request_bytes, [&](int, auto) { ++closed; });
        c.start();
        p.send(request("/big") + request("/b") + request("/c"));
        loop.poll_once(100);
        expect(C::result(c).write_would_block && calls == 1,
               "FIN starts during actual EAGAIN");
        ::shutdown(p.peer.fd(), SHUT_WR);
        const auto wanted = response(big) + response("/b") + response("/c");
        expect(pump(loop, p, c, wanted.size()) == wanted,
               "FIN cannot discard buffered complete requests");
        for (int i = 0; i < 5; ++i)
            loop.poll_once(0);
        expect(closed == 1 && calls == 3 && p.collect().empty(),
               "slow FIN all responses once before single close");
    }
    std::cout
        << "rework: real_service400_first_second=2 suffix_provider=0 provider_relax500=1 slow_FIN_EAGAIN=1 FIN_responses=3\n";
}

void generic_drains() {
    for (int mode = 0; mode < 3; ++mode) {
        EventLoop loop;
        Pair p;
        int notifications{}, depth{}, max_depth{}, closed{}, alive{};
        TcpConnection c(
            loop, std::move(p.owner), 3, [](TcpConnection&, auto, bool) {},
            http::max_request_bytes, [&](int, auto) { ++closed; });
        c.start();
        c.set_write_complete_callback([&](TcpConnection& conn) {
            ++depth;
            max_depth = std::max(max_depth, depth);
            ++notifications;
            if (notifications == 1)
                conn.send(bytes("two"));
            else if (mode == 0)
                conn.close_after_flush();
            else if (mode == 1) {
                conn.request_close();
                alive = ::fcntl(conn.fd(), F_GETFD) != -1;
            } else {
                --depth;
                throw std::runtime_error("write complete");
            }
            --depth;
        });
        for (int i = 0; i < 5; ++i) {
            loop.poll_once(0);
        }
        expect(notifications == 0, "initial empty poll does not notify");
        c.send(bytes("one"));
        expect(notifications == 0,
               "event-external send never synchronously notifies");
        auto got = pump(loop, p, c, 6);
        for (int i = 0; i < 10; ++i)
            loop.poll_once(0);
        expect(got == "onetwo" && notifications == 2 && max_depth == 1 &&
                   closed == 1,
               "drain enqueue/close/throw is once, iterative, isolated");
        if (mode == 1)
            expect(alive == 1,
                   "fd remains alive after callback requests close");
        std::cout << "drain mode=" << mode << " notifications=" << notifications
                  << " max_depth=" << max_depth << " close=" << closed
                  << " callback_alive=" << alive << '\n';
    }
    {
        EventLoop loop;
        Pair p;
        bool returned = false, requested = false;
        int destroyed = 0, alive = 0;
        auto c = std::make_unique<TcpConnection>(
            loop, std::move(p.owner), 7, [](TcpConnection&, auto, bool) {}, 0,
            [&](int, auto) { requested = true; });
        const int fd = c->fd();
        loop.set_after_dispatch([&] {
            if (requested) {
                expect(returned, "owner destroys after drain callback return");
                c.reset();
                ++destroyed;
            }
        });
        c->set_write_complete_callback([&](TcpConnection& conn) {
            conn.request_close();
            alive = ::fcntl(conn.fd(), F_GETFD) != -1;
            returned = true;
        });
        c->start();
        c->send(bytes("done"));
        loop.poll_once(100);
        expect(alive == 1 && destroyed == 1 && !c &&
                   ::fcntl(fd, F_GETFD) == -1 && p.collect() == "done",
               "drain callback deferred destruction preserves live object");
        std::cout << "drain destruction: callback_alive=" << alive
                  << " returned=" << returned << " destroyed=" << destroyed
                  << "\n";
    }
}

void boundary_rejection_and_fin() {
    std::size_t rejected{}, truncated{};
    for (const auto& sample : protocol_cases::rejections())
        for (bool prefix : {false, true}) {
            EventLoop loop;
            Pair pair;
            std::size_t calls{}, suffix_calls{}, closes{};
            TcpConnection c(
                loop, std::move(pair.owner), 81,
                app::make_http_callback([&](const http::HttpRequest& r,
                                            http::ConnectionPolicy policy) {
                    ++calls;
                    if (r.target == "/suffix")
                        ++suffix_calls;
                    if (r.target == "/bad%20target")
                        return http::ResponseResult{
                            http::make_error_response(
                                http::Status::bad_request),
                            http::ConnectionPolicy::close};
                    return http::ResponseResult{
                        http::make_response(http::Status::ok, bytes(r.target),
                                            "text/plain", false, policy),
                        policy};
                }),
                http::max_request_bytes, [&](int, auto) { ++closes; });
            c.start();
            pair.send((prefix ? request("/prefix") : "") + sample.wire +
                      request("/suffix"));
            const auto expected =
                (prefix ? response("/prefix") : "") +
                text(http::make_error_response(
                    sample.status == 405 ? http::Status::method_not_allowed
                                         : http::Status::bad_request));
            expect(pump(loop, pair, c, expected.size()) == expected,
                   "S3 component rejection ordered bytes");
            for (int i = 0; i < 5; ++i)
                loop.poll_once(0);
            expect(calls == static_cast<std::size_t>(prefix) +
                                sample.reaches_service &&
                       suffix_calls == 0 && closes == 1 &&
                       pair.collect().empty(),
                   "S3 rejection invokes no suffix and closes once");
            std::cout << "S3 component reject=" << sample.id
                      << " position=" << (prefix ? 2 : 1)
                      << " provider=" << calls << " suffix=" << suffix_calls
                      << " closes=" << closes << '\n';
            ++rejected;
        }
    for (bool reused : {false, true})
        for (std::size_t cut = 1; cut < protocol_cases::short_request.size();
             ++cut) {
            EventLoop loop;
            Pair pair;
            std::size_t calls{}, closes{};
            TcpConnection c(
                loop, std::move(pair.owner), 82,
                app::make_http_callback([&](const http::HttpRequest& r,
                                            http::ConnectionPolicy policy) {
                    ++calls;
                    return http::ResponseResult{
                        http::make_response(http::Status::ok, bytes(r.target),
                                            "text/plain", false, policy),
                        policy};
                }),
                http::max_request_bytes, [&](int, auto) { ++closes; });
            c.start();
            if (reused) {
                pair.send(request("/prefix"));
                expect(pump(loop, pair, c, response("/prefix").size()) ==
                           response("/prefix"),
                       "successful prefix before truncation");
            }
            pair.send(
                std::string_view(protocol_cases::short_request).substr(0, cut));
            loop.poll_once(100);
            expect(pair.collect().empty(),
                   "incomplete prefix has no early response");
            expect(::shutdown(pair.peer.fd(), SHUT_WR) == 0, "component FIN");
            const auto expected =
                text(http::make_error_response(http::Status::bad_request));
            expect(pump(loop, pair, c, expected.size()) == expected,
                   "truncated prefix produces exactly400");
            for (int i = 0; i < 5; ++i)
                loop.poll_once(0);
            expect(calls == static_cast<std::size_t>(reused) && closes == 1 &&
                       pair.collect().empty(),
                   "every FIN prefix closes once without provider");
            ++truncated;
        }
    std::cout << "S3 component rejection_cases=" << rejected
              << " FIN_prefix_cases=" << truncated
              << " duplicate_close=0 suffix_calls=0\n";
}

}

int main() {
    try {
        pipeline_slow_and_repeated();
        terminal_matrix_and_eof();
        service_policy_and_fin();
        generic_drains();
        boundary_rejection_and_fin();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    std::cout << "keep_alive failures=" << failures << '\n';
    return failures ? 1 : 0;
}
