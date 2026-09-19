// Reuse the existing production-process/fixture harness; its regression entry
// remains a separate CTest identity. This target executes only the tests below.
#define main legacy_http_integration_entry
#include "http_server_integration_tests.cpp"
#undef main
#include "http_protocol_boundary_cases.h"

namespace {
// Leader S3-report-003: only a fully checked oversized response may end in
// reset.
bool LimitTerminationOk(std::string_view pending_,
                        ssize_t received,
                        int error) {
  return pending_.empty() &&
         (received == 0 || (received == -1 && error == ECONNRESET));
}

class Client {
 public:
  explicit Client(std::uint16_t port) : fd_(ConnectClient(port)) {}

  struct AdoptSocket {};

  Client(int socket, AdoptSocket) : fd_(socket) {}

  ~Client() { ::close(fd_); }

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  int fd_;
  std::string pending_;

  struct Message {
    int status;
    std::string header, body;
  };

  void ReadMoreResponseBytes() {
    char buffer[32768];
    auto n = ::recv(fd_, buffer, sizeof(buffer), 0);
    if (n <= 0)
      throw std::runtime_error(
          "response ended or timed out before Content-Length boundary");
    pending_.append(buffer, static_cast<std::size_t>(n));
  }

  Message ReadResponse() {
    while (pending_.find("\r\n\r\n") == std::string::npos)
      ReadMoreResponseBytes();
    const auto boundary = pending_.find("\r\n\r\n") + 4;
    const auto header = pending_.substr(0, boundary);
    const std::string key = "Content-Length: ";
    const auto pos = header.find(key);
    Expect(pos != std::string::npos &&
               header.find(key, pos + 1) == std::string::npos,
           "unique Content-Length");
    const auto length = std::stoull(header.substr(pos + key.size()));
    while (pending_.size() < boundary + length) ReadMoreResponseBytes();
    auto body = pending_.substr(boundary, length);
    pending_.erase(0, boundary + length);
    const auto connection = header.find("Connection: ");
    Expect(connection != std::string::npos &&
               header.find("Connection: ", connection + 1) == std::string::npos,
           "unique Connection");
    return {std::stoi(header.substr(9, 3)), header, std::move(body)};
  }

  std::string LimitEnd() {
    if (!pending_.empty())
      throw std::runtime_error("extra bytes after complete oversized response");
    char byte;
    const auto n = ::recv(fd_, &byte, 1, 0);
    const int error = errno;
    if (!LimitTerminationOk(pending_, n, error))
      throw std::runtime_error(
          "oversized response did not terminate cleanly or reset after "
          "completion");
    return n == 0 ? "EOF" : "ECONNRESET";
  }

  void ExpectEof() {
    Expect(pending_.empty(), "no extra pipeline response");
    char b;
    Expect(::recv(fd_, &b, 1, 0) == 0,
           "terminal response followed by real EOF");
  }
};

std::string Get(std::string_view target, std::string_view headers = "") {
  return "GET " + std::string(target) + " HTTP/1.1\r\nHost: localhost\r\n" +
         std::string(headers) + "\r\n";
}

void Check(const Client::Message& m,
           int status,
           std::string_view body,
           bool close) {
  Expect(m.status == status && m.body == body, "ordered exact status and body");
  Expect(
      m.header.find(close ? "Connection: close\r\n"
                          : "Connection: keep-alive\r\n") != std::string::npos,
      "wire connection policy");
}

void Reuse(std::uint16_t port) {
  const std::vector<std::string> targets = {"/",
                                            "/note.txt",
                                            "/assets/unknown.blob"};
  const std::vector<std::string> bodies = {"<h1>integration index</h1>\n",
                                           "hello from S3\n",
                                           "unknown mime\n"};
  {
    Client c(port);
    for (std::size_t i = 0; i < 3; ++i) {
      SendAll(c.fd_, Get(targets[i], i == 2 ? "Connection: close\r\n" : ""));
      Check(c.ReadResponse(), 200, bodies[i], i == 2);
    }
    c.ExpectEof();
    std::cout << "sequential: client_fd=" << c.fd_
              << " connection=1 responses=3 reconnect=0\n";
  }
  {
    Client c(port);
    SendAll(c.fd_,
            Get(targets[0]) + Get(targets[1]) +
                Get(targets[2], "Connection: close\r\n"));
    for (std::size_t i = 0; i < 3; ++i)
      Check(c.ReadResponse(), 200, bodies[i], i == 2);
    c.ExpectEof();
    std::cout << "pipeline: connection=1 single_write=1 responses=3 "
                 "order=index,note,unknown\n";
  }
  {
    Client c(port);
    SendAll(c.fd_,
            Get(targets[0]) + Get(targets[1]) +
                "GET /assets/unknown.blob HTTP/1.1\r\nHo");
    Check(c.ReadResponse(), 200, bodies[0], false);
    Check(c.ReadResponse(), 200, bodies[1], false);
    Expect(c.pending_.empty(), "incomplete third has no response");
    SendAll(c.fd_, "st: x\r\nConnection: close\r\n\r\n");
    Check(c.ReadResponse(), 200, bodies[2], true);
    c.ExpectEof();
  }
  {
    Client c(port);
    SendAll(c.fd_, Get("/") + Get("/note.txt") + Get("/assets/unknown.blob"));
    ::shutdown(c.fd_, SHUT_WR);
    for (std::size_t i = 0; i < 3; ++i)
      Check(c.ReadResponse(), 200, bodies[i], false);
    c.ExpectEof();
  }
  {
    Client c(port);
    SendAll(c.fd_, Get("/"));
    Check(c.ReadResponse(), 200, bodies[0], false);
    ::shutdown(c.fd_, SHUT_WR);
    c.ExpectEof();
  }
  {
    Client c(port);
    SendAll(c.fd_, Get("/") + "GET /next HTTP/1.1\r\n");
    ::shutdown(c.fd_, SHUT_WR);
    Check(c.ReadResponse(), 200, bodies[0], false);
    Check(c.ReadResponse(), 400, "400 Bad Request\n", true);
    c.ExpectEof();
  }
  for (bool prefix : {false, true}) {
    Client c(port);
    SendAll(c.fd_,
            (prefix ? Get("/") : "") + Get("/bad%20target") + Get("/note.txt"));
    if (prefix) Check(c.ReadResponse(), 200, bodies[0], false);
    Check(c.ReadResponse(), 400, "400 Bad Request\n", true);
    c.ExpectEof();
  }
  for (const auto& target : {"/../sibling-secret.txt", "/missing"}) {
    Client c(port);
    SendAll(c.fd_, Get(target) + Get("/note.txt", "Connection: close\r\n"));
    const auto m = c.ReadResponse();
    Expect(m.status == (std::string_view(target) == "/missing" ? 404 : 403) &&
               m.header.find("keep-alive") != std::string::npos,
           "normal 403/404 retain reuse");
    Check(c.ReadResponse(), 200, bodies[1], true);
    c.ExpectEof();
  }
  for (const auto& header :
       {"Connection: ClOsE, keep-alive\r\nConnection: keep-alive\r\n",
        "Content-Length: 9\r\n",
        "Transfer-Encoding: chunked\r\n",
        "Expect: 100-continue\r\n"}) {
    Client c(port);
    SendAll(c.fd_, Get("/note.txt", header) + Get("/"));
    const bool normal = std::string_view(header).starts_with("Connection");
    Check(c.ReadResponse(),
          normal ? 200 : 400,
          normal ? bodies[1] : "400 Bad Request\n",
          true);
    c.ExpectEof();
  }
  std::cout << "production: partial_third=1 FIN_pipeline=3 idle_EOF=1 "
               "partial_EOF400=1 "
               "service400_suffix=2 reusable403_404=2 terminal_matrix=4\n";
}

void TerminationGuardTests() {
  Expect(LimitTerminationOk("", 0, 0) && LimitTerminationOk("", -1, ECONNRESET),
         "two allowed post-response endings");
  Expect(!LimitTerminationOk("x", 0, 0) && !LimitTerminationOk("", 1, 0),
         "extra pending or newly received byte rejected");
  for (int error : {EAGAIN, EWOULDBLOCK, EIO, ETIMEDOUT, EINTR})
    Expect(!LimitTerminationOk("", -1, error),
           "other errors never accepted as reset");
  for (bool short_body : {true, false}) {
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0)
      throw std::runtime_error("termination fixture socketpair");
    Client client(fds[0], Client::AdoptSocket{});
    Client writer(fds[1], Client::AdoptSocket{});
    SendAll(writer.fd_,
            std::string("HTTP/1.1 400 Bad Request\r\nContent-Length: "
                        "5\r\nConnection: close\r\n\r\n") +
                (short_body ? "er" : "errorx"));
    Expect(::shutdown(writer.fd_, SHUT_WR) == 0,
           "termination fixture shutdown");
    bool rejected = false;
    try {
      (void)client.ReadResponse();
      if (short_body)
        throw std::logic_error("short response incorrectly parsed");
      (void)client.LimitEnd();
    } catch (const std::logic_error&) {
      throw;
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    Expect(rejected,
           short_body ? "short response fails before termination rule"
                      : "extra response byte fails termination rule");
  }
  std::cout << "S3 termination_guard short_body=reject extra_bytes=reject "
               "other_errors=reject "
               "strict_EOF_unchanged=1\n";
}

void RejectionPositions(ServerProcess& server) {
  for (const auto& sample : protocol_cases::Rejections())
    for (bool prefix : {false, true}) {
      const int before = failures;
      Client c(server.port());
      SendAll(c.fd_,
              (prefix ? Get("/note.txt") : "") + sample.wire +
                  Get("/assets/unknown.blob"));
      if (prefix) Check(c.ReadResponse(), 200, "hello from S3\n", false);
      const auto message = c.ReadResponse();
      Check(message,
            sample.status,
            sample.status == 405 ? "405 Method Not Allowed\n"
                                 : "400 Bad Request\n",
            true);
      if (sample.status == 405)
        Expect(message.header.find("Allow: GET\r\n") != std::string::npos,
               "405 Allow GET");
      std::string ending = "EOF";
      if (sample.permits_reset_after_complete_response)
        ending = c.LimitEnd();
      else
        c.ExpectEof();
      std::cout << "S3 reject id=" << sample.id
                << " position=" << (prefix ? 2 : 1)
                << " responses=" << (prefix ? 2 : 1) << " ending=" << ending
                << " extra_bytes=0 suffix=0 failures=" << failures - before
                << '\n';
      server.DrainOutput(std::chrono::milliseconds(1));
    }
}

void FinPrefixes(ServerProcess& server) {
  std::size_t cases{};
  for (bool reused : {false, true})
    for (std::size_t cut = 1; cut < protocol_cases::kShortRequest.size();
         ++cut) {
      const int before = failures;
      Client c(server.port());
      if (reused) {
        SendAll(c.fd_, Get("/note.txt"));
        Check(c.ReadResponse(), 200, "hello from S3\n", false);
      }
      SendAll(c.fd_,
              std::string_view(protocol_cases::kShortRequest).substr(0, cut));
      Expect(::shutdown(c.fd_, SHUT_WR) == 0, "prefix FIN sent");
      Check(c.ReadResponse(), 400, "400 Bad Request\n", true);
      c.ExpectEof();
      ++cases;
      std::cout << "S3 FIN cut=" << cut
                << " state=" << protocol_cases::PrefixState(cut)
                << " reused=" << reused << " responses=" << (reused ? 2 : 1)
                << " eof=1 failures=" << failures - before << '\n';
      server.DrainOutput(std::chrono::milliseconds(1));
    }
  {
    Client c(server.port());
    Expect(::shutdown(c.fd_, SHUT_WR) == 0, "initial empty FIN");
    Check(c.ReadResponse(), 400, "400 Bad Request\n", true);
    c.ExpectEof();
  }
  std::cout << "S3 FIN prefix_cases=" << cases
            << " initial_empty=1 idle_and_complete_pipeline=reused_legacy\n";
}

bool WaitFdBaseline(ServerProcess& server, std::size_t expected) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < until) {
    server.DrainOutput(std::chrono::milliseconds(20));
    if (server.fd_count() == expected) return true;
  }
  return false;
}

void ResetIncomplete(ServerProcess& server, std::size_t baseline) {
  {
    Client incomplete(server.port());
    SendAll(incomplete.fd_, "GET / HTTP/1.1\r\nHost: ");
    // A separate successful exchange is an event-loop progress barrier.
    {
      Client barrier(server.port());
      SendAll(barrier.fd_, Get("/note.txt", "Connection: close\r\n"));
      Check(barrier.ReadResponse(), 200, "hello from S3\n", true);
      barrier.ExpectEof();
    }
    Expect(WaitFdBaseline(server, baseline + 1),
           "incomplete request remains the sole active connection");
    pollfd descriptor{incomplete.fd_, POLLIN, 0};
    Expect(::poll(&descriptor, 1, 0) == 0,
           "incomplete request has no premature response");
    linger reset{1, 0};
    Expect(::setsockopt(incomplete.fd_,
                        SOL_SOCKET,
                        SO_LINGER,
                        &reset,
                        sizeof(reset)) == 0,
           "incomplete RST enabled");
  }
  for (int i = 0; i < 20; ++i) {
    Client c(server.port());
    SendAll(c.fd_, Get("/note.txt", "Connection: close\r\n"));
    Check(c.ReadResponse(), 200, "hello from S3\n", true);
    c.ExpectEof();
  }
  Expect(WaitFdBaseline(server, baseline),
         "incomplete RST fd recovery by deadline");
  std::cout << "S3 RST state=incomplete survivors=20 fd_before=" << baseline
            << " fd_after=" << server.fd_count() << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  try {
    TerminationGuardTests();
    Fixture fixture;
    auto server = StartServer(argv[1], fixture.root_);
    const auto before = server.fd_count();
    Reuse(server.port());
    RejectionPositions(server);
    FinPrefixes(server);
    Expect(WaitFdBaseline(server, before), "pre-reset fd baseline");
    ResetIncomplete(server, before);
    // Reset a paused large response, then prove other connections remain
    // usable.
    {
      Client c(server.port());
      const auto start = server.output().size();
      SendAll(c.fd_, Get("/large.bin") + Get("/note.txt"));
      const auto until =
          std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (server.output().find("write reached EAGAIN", start) ==
                 std::string::npos &&
             std::chrono::steady_clock::now() < until)
        server.DrainOutput(std::chrono::milliseconds(20));
      Expect(server.output().find("write reached EAGAIN", start) !=
                 std::string::npos,
             "fresh EAGAIN before large-response RST");
      linger reset{1, 0};
      Expect(
          ::setsockopt(c.fd_, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) ==
              0,
          "large response RST enabled");
    }
    for (int i = 0; i < 20; ++i) {
      Client c(server.port());
      SendAll(c.fd_, Get("/note.txt", "Connection: close\r\n"));
      Check(c.ReadResponse(), 200, "hello from S3\n", true);
      c.ExpectEof();
    }
    Expect(WaitFdBaseline(server, before), "large RST deadline fd recovery");
    const auto after = server.fd_count();
    Expect(before == after,
           "production fd baseline restored after reset and 20 survivors");
    Expect(server.output().find("write reached EAGAIN") != std::string::npos,
           "reset targeted genuinely blocked large output");
    std::cout << "reset: blocked_EAGAIN=1 survivors=20 fd_before=" << before
              << " fd_after=" << after << " failures=" << failures << '\n';
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
  return failures ? 1 : 0;
}
