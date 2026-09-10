// Reuse the existing production-process/fixture harness; its regression entry
// remains a separate CTest identity. This target executes only the tests below.
#define main legacy_http_integration_entry
#include "http_server_integration_tests.cpp"
#undef main
#include "http_protocol_boundary_cases.h"

namespace {
// Leader S3-report-003: only a fully checked oversized response may end in reset.
bool limit_termination_ok(std::string_view pending, ssize_t received,
                          int error) {
    return pending.empty() &&
           (received == 0 || (received == -1 && error == ECONNRESET));
}

class Client {
public:
    explicit Client(std::uint16_t port) : fd(connect_client(port)) {}

    struct AdoptSocket {};

    Client(int socket, AdoptSocket) : fd(socket) {}

    ~Client() { ::close(fd); }

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    int fd;
    std::string pending;

    struct Message {
        int status;
        std::string header, body;
    };

    Message next() {
        const auto read_more = [&] {
            char buffer[32768];
            auto n = ::recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0)
                throw std::runtime_error(
                    "response ended or timed out before Content-Length boundary");
            pending.append(buffer, static_cast<std::size_t>(n));
        };
        while (pending.find("\r\n\r\n") == std::string::npos)
            read_more();
        const auto boundary = pending.find("\r\n\r\n") + 4;
        const auto header = pending.substr(0, boundary);
        const std::string key = "Content-Length: ";
        const auto pos = header.find(key);
        expect(pos != std::string::npos &&
                   header.find(key, pos + 1) == std::string::npos,
               "unique Content-Length");
        const auto length = std::stoull(header.substr(pos + key.size()));
        while (pending.size() < boundary + length)
            read_more();
        auto body = pending.substr(boundary, length);
        pending.erase(0, boundary + length);
        const auto connection = header.find("Connection: ");
        expect(connection != std::string::npos &&
                   header.find("Connection: ", connection + 1) ==
                       std::string::npos,
               "unique Connection");
        return {std::stoi(header.substr(9, 3)), header, std::move(body)};
    }

    std::string limit_end() {
        if (!pending.empty())
            throw std::runtime_error(
                "extra bytes after complete oversized response");
        char byte;
        const auto n = ::recv(fd, &byte, 1, 0);
        const int error = errno;
        if (!limit_termination_ok(pending, n, error))
            throw std::runtime_error(
                "oversized response did not terminate cleanly or reset after completion");
        return n == 0 ? "EOF" : "ECONNRESET";
    }

    void eof() {
        expect(pending.empty(), "no extra pipeline response");
        char b;
        expect(::recv(fd, &b, 1, 0) == 0,
               "terminal response followed by real EOF");
    }
};

std::string get(std::string_view target, std::string_view headers = "") {
    return "GET " + std::string(target) + " HTTP/1.1\r\nHost: localhost\r\n" +
           std::string(headers) + "\r\n";
}

void check(const Client::Message& m, int status, std::string_view body,
           bool close) {
    expect(m.status == status && m.body == body,
           "ordered exact status and body");
    expect(m.header.find(close ? "Connection: close\r\n"
                               : "Connection: keep-alive\r\n") !=
               std::string::npos,
           "wire connection policy");
}

void reuse(std::uint16_t port) {
    const std::vector<std::string> targets = {"/", "/note.txt",
                                              "/assets/unknown.blob"};
    const std::vector<std::string> bodies = {
        "<h1>integration index</h1>\n", "hello from S3\n", "unknown mime\n"};
    {
        Client c(port);
        for (std::size_t i = 0; i < 3; ++i) {
            send_all(c.fd,
                     get(targets[i], i == 2 ? "Connection: close\r\n" : ""));
            check(c.next(), 200, bodies[i], i == 2);
        }
        c.eof();
        std::cout << "sequential: client_fd=" << c.fd
                  << " connection=1 responses=3 reconnect=0\n";
    }
    {
        Client c(port);
        send_all(c.fd, get(targets[0]) + get(targets[1]) +
                           get(targets[2], "Connection: close\r\n"));
        for (std::size_t i = 0; i < 3; ++i)
            check(c.next(), 200, bodies[i], i == 2);
        c.eof();
        std::cout
            << "pipeline: connection=1 single_write=1 responses=3 order=index,note,unknown\n";
    }
    {
        Client c(port);
        send_all(c.fd, get(targets[0]) + get(targets[1]) +
                           "GET /assets/unknown.blob HTTP/1.1\r\nHo");
        check(c.next(), 200, bodies[0], false);
        check(c.next(), 200, bodies[1], false);
        expect(c.pending.empty(), "incomplete third has no response");
        send_all(c.fd, "st: x\r\nConnection: close\r\n\r\n");
        check(c.next(), 200, bodies[2], true);
        c.eof();
    }
    {
        Client c(port);
        send_all(c.fd,
                 get("/") + get("/note.txt") + get("/assets/unknown.blob"));
        ::shutdown(c.fd, SHUT_WR);
        for (std::size_t i = 0; i < 3; ++i)
            check(c.next(), 200, bodies[i], false);
        c.eof();
    }
    {
        Client c(port);
        send_all(c.fd, get("/"));
        check(c.next(), 200, bodies[0], false);
        ::shutdown(c.fd, SHUT_WR);
        c.eof();
    }
    {
        Client c(port);
        send_all(c.fd, get("/") + "GET /next HTTP/1.1\r\n");
        ::shutdown(c.fd, SHUT_WR);
        check(c.next(), 200, bodies[0], false);
        check(c.next(), 400, "400 Bad Request\n", true);
        c.eof();
    }
    for (bool prefix : {false, true}) {
        Client c(port);
        send_all(c.fd, (prefix ? get("/") : "") + get("/bad%20target") +
                           get("/note.txt"));
        if (prefix)
            check(c.next(), 200, bodies[0], false);
        check(c.next(), 400, "400 Bad Request\n", true);
        c.eof();
    }
    for (const auto& target : {"/../sibling-secret.txt", "/missing"}) {
        Client c(port);
        send_all(c.fd, get(target) + get("/note.txt", "Connection: close\r\n"));
        const auto m = c.next();
        expect(
            m.status == (std::string_view(target) == "/missing" ? 404 : 403) &&
                m.header.find("keep-alive") != std::string::npos,
            "normal 403/404 retain reuse");
        check(c.next(), 200, bodies[1], true);
        c.eof();
    }
    for (const auto& header :
         {"Connection: ClOsE, keep-alive\r\nConnection: keep-alive\r\n",
          "Content-Length: 9\r\n", "Transfer-Encoding: chunked\r\n",
          "Expect: 100-continue\r\n"}) {
        Client c(port);
        send_all(c.fd, get("/note.txt", header) + get("/"));
        const bool normal = std::string_view(header).starts_with("Connection");
        check(c.next(), normal ? 200 : 400,
              normal ? bodies[1] : "400 Bad Request\n", true);
        c.eof();
    }
    std::cout
        << "production: partial_third=1 FIN_pipeline=3 idle_EOF=1 partial_EOF400=1 service400_suffix=2 reusable403_404=2 terminal_matrix=4\n";
}

void termination_guard_tests() {
    expect(limit_termination_ok("", 0, 0) &&
               limit_termination_ok("", -1, ECONNRESET),
           "two allowed post-response endings");
    expect(!limit_termination_ok("x", 0, 0) && !limit_termination_ok("", 1, 0),
           "extra pending or newly received byte rejected");
    for (int error : {EAGAIN, EWOULDBLOCK, EIO, ETIMEDOUT, EINTR})
        expect(!limit_termination_ok("", -1, error),
               "other errors never accepted as reset");
    for (bool short_body : {true, false}) {
        int fds[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0)
            throw std::runtime_error("termination fixture socketpair");
        Client client(fds[0], Client::AdoptSocket{});
        Client writer(fds[1], Client::AdoptSocket{});
        send_all(
            writer.fd,
            std::string(
                "HTTP/1.1 400 Bad Request\r\nContent-Length: 5\r\nConnection: close\r\n\r\n") +
                (short_body ? "er" : "errorx"));
        expect(::shutdown(writer.fd, SHUT_WR) == 0,
               "termination fixture shutdown");
        bool rejected = false;
        try {
            (void)client.next();
            if (short_body)
                throw std::logic_error("short response incorrectly parsed");
            (void)client.limit_end();
        } catch (const std::logic_error&) {
            throw;
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        expect(rejected, short_body
                             ? "short response fails before termination rule"
                             : "extra response byte fails termination rule");
    }
    std::cout
        << "S3 termination_guard short_body=reject extra_bytes=reject other_errors=reject strict_EOF_unchanged=1\n";
}

void rejection_positions(ServerProcess& server) {
    for (const auto& sample : protocol_cases::rejections())
        for (bool prefix : {false, true}) {
            const int before = failures;
            Client c(server.port());
            send_all(c.fd, (prefix ? get("/note.txt") : "") + sample.wire +
                               get("/assets/unknown.blob"));
            if (prefix)
                check(c.next(), 200, "hello from S3\n", false);
            const auto message = c.next();
            check(message, sample.status,
                  sample.status == 405 ? "405 Method Not Allowed\n"
                                       : "400 Bad Request\n",
                  true);
            if (sample.status == 405)
                expect(
                    message.header.find("Allow: GET\r\n") != std::string::npos,
                    "405 Allow GET");
            std::string ending = "EOF";
            if (sample.permits_reset_after_complete_response)
                ending = c.limit_end();
            else
                c.eof();
            std::cout << "S3 reject id=" << sample.id
                      << " position=" << (prefix ? 2 : 1)
                      << " responses=" << (prefix ? 2 : 1)
                      << " ending=" << ending
                      << " extra_bytes=0 suffix=0 failures="
                      << failures - before << '\n';
            server.drain_output(std::chrono::milliseconds(1));
        }
}

void fin_prefixes(ServerProcess& server) {
    std::size_t cases{};
    for (bool reused : {false, true})
        for (std::size_t cut = 1; cut < protocol_cases::short_request.size();
             ++cut) {
            const int before = failures;
            Client c(server.port());
            if (reused) {
                send_all(c.fd, get("/note.txt"));
                check(c.next(), 200, "hello from S3\n", false);
            }
            send_all(
                c.fd,
                std::string_view(protocol_cases::short_request).substr(0, cut));
            expect(::shutdown(c.fd, SHUT_WR) == 0, "prefix FIN sent");
            check(c.next(), 400, "400 Bad Request\n", true);
            c.eof();
            ++cases;
            std::cout << "S3 FIN cut=" << cut
                      << " state=" << protocol_cases::prefix_state(cut)
                      << " reused=" << reused
                      << " responses=" << (reused ? 2 : 1)
                      << " eof=1 failures=" << failures - before << '\n';
            server.drain_output(std::chrono::milliseconds(1));
        }
    {
        Client c(server.port());
        expect(::shutdown(c.fd, SHUT_WR) == 0, "initial empty FIN");
        check(c.next(), 400, "400 Bad Request\n", true);
        c.eof();
    }
    std::cout << "S3 FIN prefix_cases=" << cases
              << " initial_empty=1 idle_and_complete_pipeline=reused_legacy\n";
}

bool wait_fd_baseline(ServerProcess& server, std::size_t expected) {
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < until) {
        server.drain_output(std::chrono::milliseconds(20));
        if (server.fd_count() == expected)
            return true;
    }
    return false;
}

void reset_incomplete(ServerProcess& server, std::size_t baseline) {
    {
        Client incomplete(server.port());
        send_all(incomplete.fd, "GET / HTTP/1.1\r\nHost: ");
        // A separate successful exchange is an event-loop progress barrier.
        {
            Client barrier(server.port());
            send_all(barrier.fd, get("/note.txt", "Connection: close\r\n"));
            check(barrier.next(), 200, "hello from S3\n", true);
            barrier.eof();
        }
        expect(wait_fd_baseline(server, baseline + 1),
               "incomplete request remains the sole active connection");
        pollfd descriptor{incomplete.fd, POLLIN, 0};
        expect(::poll(&descriptor, 1, 0) == 0,
               "incomplete request has no premature response");
        linger reset{1, 0};
        expect(::setsockopt(incomplete.fd, SOL_SOCKET, SO_LINGER, &reset,
                            sizeof(reset)) == 0,
               "incomplete RST enabled");
    }
    for (int i = 0; i < 20; ++i) {
        Client c(server.port());
        send_all(c.fd, get("/note.txt", "Connection: close\r\n"));
        check(c.next(), 200, "hello from S3\n", true);
        c.eof();
    }
    expect(wait_fd_baseline(server, baseline),
           "incomplete RST fd recovery by deadline");
    std::cout << "S3 RST state=incomplete survivors=20 fd_before=" << baseline
              << " fd_after=" << server.fd_count() << '\n';
}

}

int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    try {
        termination_guard_tests();
        Fixture fixture;
        auto server = start_server(argv[1], fixture.root);
        const auto before = server.fd_count();
        reuse(server.port());
        rejection_positions(server);
        fin_prefixes(server);
        expect(wait_fd_baseline(server, before), "pre-reset fd baseline");
        reset_incomplete(server, before);
        // Reset a paused large response, then prove other connections remain usable.
        {
            Client c(server.port());
            const auto start = server.output().size();
            send_all(c.fd, get("/large.bin") + get("/note.txt"));
            const auto until =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (server.output().find("write reached EAGAIN", start) ==
                       std::string::npos &&
                   std::chrono::steady_clock::now() < until)
                server.drain_output(std::chrono::milliseconds(20));
            expect(server.output().find("write reached EAGAIN", start) !=
                       std::string::npos,
                   "fresh EAGAIN before large-response RST");
            linger reset{1, 0};
            expect(::setsockopt(c.fd, SOL_SOCKET, SO_LINGER, &reset,
                                sizeof(reset)) == 0,
                   "large response RST enabled");
        }
        for (int i = 0; i < 20; ++i) {
            Client c(server.port());
            send_all(c.fd, get("/note.txt", "Connection: close\r\n"));
            check(c.next(), 200, "hello from S3\n", true);
            c.eof();
        }
        expect(wait_fd_baseline(server, before),
               "large RST deadline fd recovery");
        const auto after = server.fd_count();
        expect(before == after,
               "production fd baseline restored after reset and 20 survivors");
        expect(
            server.output().find("write reached EAGAIN") != std::string::npos,
            "reset targeted genuinely blocked large output");
        std::cout << "reset: blocked_EAGAIN=1 survivors=20 fd_before=" << before
                  << " fd_after=" << after << " failures=" << failures << '\n';
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
    return failures ? 1 : 0;
}
