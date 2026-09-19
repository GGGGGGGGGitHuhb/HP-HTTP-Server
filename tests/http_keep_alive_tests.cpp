#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <type_traits>

#include "http_connection_handler.h"
#include "http_protocol_boundary_cases.h"

namespace hp::net {
struct ConnectionIoTestAccess {
  static auto capacity(const ConnectionIo& io) { return io.output_.capacity(); }
};

struct TcpConnectionTestAccess {
  static auto capacity(const TcpConnection& c) {
    return ConnectionIoTestAccess::capacity(c.io_);
  }

  static auto result(TcpConnection& c) { return c.last_result_; }

  static auto events(TcpConnection& c) { return c.event_count_; }

  static auto interest(TcpConnection& c) {
    return c.connection_channel_.interest();
  }
};
}  // namespace hp::net

namespace {
using namespace hp;
using namespace hp::net;
using C = TcpConnectionTestAccess;
int failures{};

void Expect(bool ok, const char* why) {
  if (!ok) {
    ++failures;
    std::cerr << "FAIL: " << why << '\n';
  }
}

std::span<const std::byte> Bytes(std::string_view s) {
  return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

std::string Text(const std::vector<std::byte>& v) {
  return {reinterpret_cast<const char*>(v.data()), v.size()};
}

std::string Request(std::string_view target = "/a",
                    std::string_view header = "") {
  return "GET " + std::string(target) + " HTTP/1.1\r\nHost: a\r\n" +
         std::string(header) + "\r\n";
}

std::string Response(
    std::string_view body,
    http::ConnectionPolicy policy = http::ConnectionPolicy::kKeepAlive) {
  return Text(http::MakeResponse(http::Status::kOk,
                                 Bytes(body),
                                 "text/plain",
                                 false,
                                 policy));
}

struct Pair {
  Socket owner, peer;

  Pair() {
    int f[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, f))
      throw std::runtime_error("socketpair");
    owner.Reset(f[0]);
    peer.Reset(f[1]);
  }

  void Send(std::string_view s) {
    Expect(::send(peer.fd(), s.data(), s.size(), MSG_NOSIGNAL) ==
               static_cast<ssize_t>(s.size()),
           "send all controlled request bytes");
  }

  std::string Collect() {
    std::string out;
    char b[32768];
    ssize_t n;
    while ((n = ::recv(peer.fd(), b, sizeof(b), MSG_DONTWAIT)) > 0)
      out.append(b, static_cast<std::size_t>(n));
    return out;
  }
};

std::string Pump(EventLoop& loop,
                 Pair& pair,
                 TcpConnection& c,
                 std::size_t expected) {
  std::string out;
  for (int i = 0; i < 10000 && out.size() < expected; ++i) {
    loop.PollOnce(0);
    out += pair.Collect();
  }
  Expect(out.size() == expected, "bounded pump received exact response bytes");
  if (c.state() == TcpConnection::State::kActive)
    Expect(!(C::interest(c) & EPOLLOUT), "EPOLLOUT removed after drain");
  return out;
}

void PipelineSlowAndRepeated() {
  EventLoop loop;
  Pair p;
  int size = 4096;
  ::setsockopt(p.owner.fd(), SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
  const std::string big(512 * 1024, 'q');
  std::size_t calls{}, closes{}, eagain{};
  TcpConnection c(loop, std::move(p.owner), 1, http::kMaxRequestBytes);
  using ResponseScenario1State0 = decltype((calls));
  using ResponseScenario1State1 = decltype((big));
  struct ResponseScenario1 {
    ResponseScenario1State0 calls;
    ResponseScenario1State1 big;
    hp::http::ResponseResult PrepareResponse(const http::HttpRequest& r,
                                             http::ConnectionPolicy policy) {
      ++calls;
      return http::ResponseResult{
          http::MakeResponse(http::Status::kOk,
                             Bytes(r.target == "/big" ? big : r.target),
                             "text/plain",
                             false,
                             policy),
          policy};
    }
  };
  c.set_HandleMessage_callback(
      app::MakeHttpCallback(std::bind_front(&ResponseScenario1::PrepareResponse,
                                            ResponseScenario1{calls, big})));
  using OnConnectionClosedObserver1State0 = decltype((closes));
  struct OnConnectionClosedObserver1 {
    OnConnectionClosedObserver1State0 closes;
    void OnConnectionClosed(int, TcpConnection::Identity) { ++closes; }
  };
  c.set_OnConnectionClosed_callback(
      std::bind_front(&OnConnectionClosedObserver1::OnConnectionClosed,
                      OnConnectionClosedObserver1{closes}));
  c.Start();
  p.Send(Request("/big") + Request("/b") + Request("/c"));
  loop.PollOnce(100);
  Expect(calls == 1 && C::result(c).write_would_block && c.pending_bytes() > 0,
         "real EAGAIN holds only first response");
  const auto pending = c.pending_bytes(), buffered = c.input_view().size(),
             events = C::events(c);
  eagain += C::result(c).write_would_block;
  Expect(buffered == Request("/b").size() + Request("/c").size() &&
             buffered <= http::kMaxRequestBytes,
         "bounded suffix stays in transport");
  p.Send(Request("/later"));
  for (int i = 0; i < 10; ++i) loop.PollOnce(0);
  Expect(
      calls == 1 && c.input_view().size() == buffered && C::events(c) == events,
      "pause has no recv or busy event while peer writes");
  const auto wanted =
      Response(big) + Response("/b") + Response("/c") + Response("/later");
  auto got = Pump(loop, p, c, wanted.size());
  Expect(got == wanted && calls == 4 && closes == 0,
         "cached pipeline and queued kernel data progress in order without new "
         "send");
  std::size_t max_pending = pending;
  const auto capacity = C::capacity(c);
  for (int n = 0; n < 24; ++n) {
    p.Send(Request(n % 2 ? "/b" : "/big"));
    loop.PollOnce(100);
    eagain += C::result(c).write_would_block;
    max_pending = std::max(max_pending, c.pending_bytes());
    auto want = Response(n % 2 ? "/b" : big);
    Expect(Pump(loop, p, c, want.size()) == want,
           "repeated small/large requests preserve bytes");
    Expect(n % 2 ? C::capacity(c) <= 64U * 1024U : C::capacity(c) == 0,
           "idle large response releases storage while small response retains "
           "bounded storage");
  }
  Expect(C::capacity(c) <= capacity,
         "output capacity does not grow over repeated requests");
  Expect(max_pending <= Response(big).size() && calls == 28,
         "pending bound independent of request count");
  std::string many, expected_many;
  for (int i = 0; i < 600; ++i) {
    many += Request("/b");
    expected_many += Response("/b");
  }
  Expect(many.size() > http::kMaxRequestBytes,
         "cumulative connection input exceeds per-request limit");
  p.Send(many);
  Expect(
      Pump(loop, p, c, expected_many.size()) == expected_many && calls == 628,
      "600 short pipelined requests reset independent limits");
  Expect(C::capacity(c) <= capacity,
         "pipeline depth does not grow output capacity");
  const auto idle = C::events(c);
  for (int i = 0; i < 20; ++i) loop.PollOnce(0);
  Expect(idle == C::events(c), "keep-alive idle no self excitation");
  ::shutdown(p.peer.fd(), SHUT_WR);
  loop.PollOnce(100);
  Expect(closes == 1 && p.Collect().empty(),
         "idle EOF after requests closes silently once");
  std::cout << "slow: EAGAIN=" << eagain
            << " blocked_provider=1 suffix=" << buffered
            << " initial_pending=" << pending << " max_pending=" << max_pending
            << " cumulative_short_bytes=" << many.size()
            << " output_capacity=" << capacity << " requests=" << calls
            << " idle_polls=20 close=" << closes << '\n';
}

void TerminalMatrixAndEof() {
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

  using VerifyTerminalResponseTaskCasesState = decltype((cases));
  struct VerifyTerminalResponseTask {
    VerifyTerminalResponseTaskCasesState cases;
    decltype(auto) VerifyTerminalResponse(const std::string& input,
                                          const std::string& wanted,
                                          std::size_t expected_calls,
                                          bool eof) const {
      EventLoop loop;
      Pair p;
      std::size_t calls{}, closes{};
      TcpConnection c(loop, std::move(p.owner), 2, http::kMaxRequestBytes);
      using ResponseScenario2State0 = decltype((calls));
      struct ResponseScenario2 {
        ResponseScenario2State0 calls;
        hp::http::ResponseResult PrepareResponse(
            const http::HttpRequest& r,
            http::ConnectionPolicy policy) {
          ++calls;
          if (r.target == "/throw") throw std::runtime_error("provider");
          if (r.target == "/403" || r.target == "/404" || r.target == "/500")
            return http::ResponseResult{
                http::MakeErrorResponse(
                    r.target == "/403"   ? http::Status::kForbidden
                    : r.target == "/404" ? http::Status::kNotFound
                                         : http::Status::kInternalServerError,
                    policy),
                policy};
          return http::ResponseResult{http::MakeResponse(http::Status::kOk,
                                                         Bytes(r.target),
                                                         "text/plain",
                                                         false,
                                                         policy),
                                      policy};
        }
      };
      c.set_HandleMessage_callback(app::MakeHttpCallback(
          std::bind_front(&ResponseScenario2::PrepareResponse,
                          ResponseScenario2{calls})));
      using OnConnectionClosedObserver2State0 = decltype((closes));
      struct OnConnectionClosedObserver2 {
        OnConnectionClosedObserver2State0 closes;
        void OnConnectionClosed(int, TcpConnection::Identity) { ++closes; }
      };
      c.set_OnConnectionClosed_callback(
          std::bind_front(&OnConnectionClosedObserver2::OnConnectionClosed,
                          OnConnectionClosedObserver2{closes}));
      c.Start();
      if (!input.empty()) p.Send(input);
      if (eof) ::shutdown(p.peer.fd(), SHUT_WR);
      const auto got = Pump(loop, p, c, wanted.size());
      // EOF may require a subsequent event after the final successful response.
      for (int i = 0; i < 5; ++i) loop.PollOnce(0);
      Expect(got == wanted && p.Collect().empty() && calls == expected_calls &&
                 closes == 1,
             "terminal/EOF exact ordered stream and no suffix provider");
      ++cases;
    }
  };
  auto run = VerifyTerminalResponseTask{cases};
  const auto bad = Text(http::MakeErrorResponse(http::Status::kBadRequest));
  for (const auto& h : invalid)
    run.VerifyTerminalResponse(
        Request("/a") + Request("/bad", h) + Request("/suffix"),
        Response("/a") + bad,
        1,
        false);
  for (const auto& h :
       {"Connection: ClOsE,keep-alive\r\nConnection: keep-alive\r\n",
        "Connection: keep-alive\r\nConnection: x, close\r\n"}) {
    run.VerifyTerminalResponse(Request("/a", h) + Request("/suffix"),
                               Response("/a", http::ConnectionPolicy::kClose),
                               1,
                               false);
    run.VerifyTerminalResponse(
        Request("/a") + Request("/b", h) + Request("/suffix"),
        Response("/a") + Response("/b", http::ConnectionPolicy::kClose),
        2,
        false);
  }
  run.VerifyTerminalResponse(
      Request("/a") + "POST / HTTP/1.1\r\nHost: a\r\n\r\n" + Request("/suffix"),
      Response("/a") +
          Text(http::MakeErrorResponse(http::Status::kMethodNotAllowed)),
      1,
      false);
  run.VerifyTerminalResponse(
      Request("/a") + Request("/throw") + Request("/suffix"),
      Response("/a") +
          Text(http::MakeErrorResponse(http::Status::kInternalServerError)),
      2,
      false);
  run.VerifyTerminalResponse("", bad, 0, true);
  run.VerifyTerminalResponse(Request("/a") + "GET /b HTTP/1.1\r\nHost:",
                             Response("/a") + bad,
                             1,
                             true);
  run.VerifyTerminalResponse(Request("/a") + Request("/b") + Request("/c"),
                             Response("/a") + Response("/b") + Response("/c"),
                             3,
                             true);
  for (const auto& target : {"/403", "/404", "/500"}) {
    auto status = std::string_view(target) == "/403" ? http::Status::kForbidden
                  : std::string_view(target) == "/404"
                      ? http::Status::kNotFound
                      : http::Status::kInternalServerError;
    run.VerifyTerminalResponse(
        Request(target) + Request("/b", "Connection: close\r\n"),
        Text(http::MakeErrorResponse(status,
                                     http::ConnectionPolicy::kKeepAlive)) +
            Response("/b", http::ConnectionPolicy::kClose),
        2,
        false);
  }
  std::cout << "terminal/eof: cases=" << cases
            << " rejected_framing=" << invalid.size()
            << " suffix_calls=0 duplicate_responses=0\n";
}

void ServicePolicyAndFin() {
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
    TcpConnection c(loop, std::move(p.owner), 4, http::kMaxRequestBytes);
    using ResponseScenario3State0 = decltype((calls));
    using ResponseScenario3State1 = decltype((service));
    struct ResponseScenario3 {
      ResponseScenario3State0 calls;
      ResponseScenario3State1 service;
      hp::http::ResponseResult PrepareResponse(const http::HttpRequest& r,
                                               http::ConnectionPolicy policy) {
        ++calls;
        return service.HandleResponse(r, policy);
      }
    };
    c.set_HandleMessage_callback(app::MakeHttpCallback(
        std::bind_front(&ResponseScenario3::PrepareResponse,
                        ResponseScenario3{calls, service})));
    using OnConnectionClosedObserver3State0 = decltype((closed));
    struct OnConnectionClosedObserver3 {
      OnConnectionClosedObserver3State0 closed;
      void OnConnectionClosed(int, TcpConnection::Identity) { ++closed; }
    };
    c.set_OnConnectionClosed_callback(
        std::bind_front(&OnConnectionClosedObserver3::OnConnectionClosed,
                        OnConnectionClosedObserver3{closed}));
    c.Start();
    p.Send((prefix ? Request("/") : "") + Request("/bad%20target") +
           Request("/suffix"));
    auto wanted =
        (prefix ? Text(service.Handle({"GET", "/"},
                                      http::ConnectionPolicy::kKeepAlive))
                : "") +
        Text(http::MakeErrorResponse(http::Status::kBadRequest));
    Expect(Pump(loop, p, c, wanted.size()) == wanted &&
               calls == (prefix ? 2U : 1U) && closed == 1,
           "real service 400 metadata stops first/second request suffix");
  }
  {
    EventLoop loop;
    Pair p;
    int calls{}, closed{};
    TcpConnection c(loop, std::move(p.owner), 5, http::kMaxRequestBytes);
    using ResponseScenario4State0 = decltype((calls));
    struct ResponseScenario4 {
      ResponseScenario4State0 calls;
      hp::http::ResponseResult PrepareResponse(const http::HttpRequest&,
                                               http::ConnectionPolicy) {
        ++calls;
        return http::ResponseResult{
            http::MakeResponse(http::Status::kOk,
                               Bytes("INVALID"),
                               "text/plain",
                               false,
                               http::ConnectionPolicy::kKeepAlive),
            http::ConnectionPolicy::kKeepAlive};
      }
    };
    c.set_HandleMessage_callback(app::MakeHttpCallback(
        std::bind_front(&ResponseScenario4::PrepareResponse,
                        ResponseScenario4{calls})));
    using OnConnectionClosedObserver4State0 = decltype((closed));
    struct OnConnectionClosedObserver4 {
      OnConnectionClosedObserver4State0 closed;
      void OnConnectionClosed(int, TcpConnection::Identity) { ++closed; }
    };
    c.set_OnConnectionClosed_callback(
        std::bind_front(&OnConnectionClosedObserver4::OnConnectionClosed,
                        OnConnectionClosedObserver4{closed}));
    c.Start();
    p.Send(Request("/", "Connection: close\r\n") + Request("/suffix"));
    const auto wanted =
        Text(http::MakeErrorResponse(http::Status::kInternalServerError));
    Expect(
        Pump(loop, p, c, wanted.size()) == wanted && calls == 1 && closed == 1,
        "provider cannot relax explicit close; invalid bytes replaced by500 "
        "before send");
  }
  {
    EventLoop loop;
    Pair p;
    int small = 4096;
    ::setsockopt(p.owner.fd(), SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
    const std::string big(512 * 1024, 'F');
    int calls{}, closed{};
    TcpConnection c(loop, std::move(p.owner), 6, http::kMaxRequestBytes);
    using ResponseScenario5State0 = decltype((calls));
    using ResponseScenario5State1 = decltype((big));
    struct ResponseScenario5 {
      ResponseScenario5State0 calls;
      ResponseScenario5State1 big;
      hp::http::ResponseResult PrepareResponse(const http::HttpRequest& r,
                                               http::ConnectionPolicy policy) {
        ++calls;
        return http::ResponseResult{
            http::MakeResponse(http::Status::kOk,
                               Bytes(r.target == "/big" ? big : r.target),
                               "text/plain",
                               false,
                               policy),
            policy};
      }
    };
    c.set_HandleMessage_callback(app::MakeHttpCallback(
        std::bind_front(&ResponseScenario5::PrepareResponse,
                        ResponseScenario5{calls, big})));
    using OnConnectionClosedObserver5State0 = decltype((closed));
    struct OnConnectionClosedObserver5 {
      OnConnectionClosedObserver5State0 closed;
      void OnConnectionClosed(int, TcpConnection::Identity) { ++closed; }
    };
    c.set_OnConnectionClosed_callback(
        std::bind_front(&OnConnectionClosedObserver5::OnConnectionClosed,
                        OnConnectionClosedObserver5{closed}));
    c.Start();
    p.Send(Request("/big") + Request("/b") + Request("/c"));
    loop.PollOnce(100);
    Expect(C::result(c).write_would_block && calls == 1,
           "FIN starts during actual EAGAIN");
    ::shutdown(p.peer.fd(), SHUT_WR);
    const auto wanted = Response(big) + Response("/b") + Response("/c");
    Expect(Pump(loop, p, c, wanted.size()) == wanted,
           "FIN cannot discard buffered complete requests");
    for (int i = 0; i < 5; ++i) loop.PollOnce(0);
    Expect(closed == 1 && calls == 3 && p.Collect().empty(),
           "slow FIN all responses once before single close");
  }
  std::cout << "rework: real_service400_first_second=2 suffix_provider=0 "
               "provider_relax500=1 "
               "slow_FIN_EAGAIN=1 FIN_responses=3\n";
}

void GenericDrains() {
  for (int mode = 0; mode < 3; ++mode) {
    EventLoop loop;
    Pair p;
    int notifications{}, depth{}, max_depth{}, closed{}, alive{};
    TcpConnection c(loop, std::move(p.owner), 3, http::kMaxRequestBytes);
    struct HandleMessageObserver6 {
      void HandleMessage(TcpConnection&, std::span<const std::byte>, bool) {}
    };
    c.set_HandleMessage_callback(
        std::bind_front(&HandleMessageObserver6::HandleMessage,
                        HandleMessageObserver6{}));
    using OnConnectionClosedObserver7State0 = decltype((closed));
    struct OnConnectionClosedObserver7 {
      OnConnectionClosedObserver7State0 closed;
      void OnConnectionClosed(int, TcpConnection::Identity) { ++closed; }
    };
    c.set_OnConnectionClosed_callback(
        std::bind_front(&OnConnectionClosedObserver7::OnConnectionClosed,
                        OnConnectionClosedObserver7{closed}));
    c.Start();
    struct IterativeWriteCompletion {
      int& notifications;
      int& depth;
      int& max_depth;
      int& alive;
      int mode;

      void HandleWriteComplete(TcpConnection& conn) {
        ++depth;
        max_depth = std::max(max_depth, depth);
        ++notifications;
        if (notifications == 1)
          conn.Send(Bytes("two"));
        else if (mode == 0)
          conn.CloseAfterFlush();
        else if (mode == 1) {
          conn.RequestClose();
          alive = ::fcntl(conn.fd(), F_GETFD) != -1;
        } else {
          --depth;
          throw std::runtime_error("write complete");
        }
        --depth;
      }
    } completion{notifications, depth, max_depth, alive, mode};
    c.set_HandleWriteComplete_callback(
        std::bind_front(&IterativeWriteCompletion::HandleWriteComplete,
                        &completion));
    for (int i = 0; i < 5; ++i) {
      loop.PollOnce(0);
    }
    Expect(notifications == 0, "initial empty poll does not notify");
    c.Send(Bytes("one"));
    Expect(notifications == 0,
           "event-external send never synchronously notifies");
    auto got = Pump(loop, p, c, 6);
    for (int i = 0; i < 10; ++i) loop.PollOnce(0);
    Expect(
        got == "onetwo" && notifications == 2 && max_depth == 1 && closed == 1,
        "drain enqueue/close/throw is once, iterative, isolated");
    if (mode == 1)
      Expect(alive == 1, "fd remains alive after callback requests close");
    std::cout << "drain mode=" << mode << " notifications=" << notifications
              << " max_depth=" << max_depth << " close=" << closed
              << " callback_alive=" << alive << '\n';
  }
  {
    EventLoop loop;
    Pair p;
    bool returned = false, requested = false;
    int destroyed = 0, alive = 0;
    auto c = std::make_unique<TcpConnection>(loop, std::move(p.owner), 7, 0);
    struct DeferredCloseObserver {
      bool& requested;
      void HandleMessage(TcpConnection&, std::span<const std::byte>, bool) {}
      void OnConnectionClosed(int, TcpConnection::Identity) {
        requested = true;
      }
    };
    c->set_HandleMessage_callback(
        std::bind_front(&DeferredCloseObserver::HandleMessage,
                        DeferredCloseObserver{requested}));
    c->set_OnConnectionClosed_callback(
        std::bind_front(&DeferredCloseObserver::OnConnectionClosed,
                        DeferredCloseObserver{requested}));
    const int fd = c->fd();
    struct DeferredConnectionCleanup {
      bool& requested;
      bool& returned;
      std::unique_ptr<TcpConnection>& connection;
      int& destroyed;
      void DrainClosedConnections() {
        if (requested) {
          Expect(returned, "owner destroys after drain callback return");
          connection.reset();
          ++destroyed;
        }
      }
    } cleanup_target{requested, returned, c, destroyed};
    loop.set_DrainClosedConnections_callback(
        std::bind_front(&DeferredConnectionCleanup::DrainClosedConnections,
                        &cleanup_target));
    struct ClosingWriteCompletion {
      int& alive;
      bool& returned;

      void HandleWriteComplete(TcpConnection& conn) {
        conn.RequestClose();
        alive = ::fcntl(conn.fd(), F_GETFD) != -1;
        returned = true;
      }
    } completion{alive, returned};
    c->set_HandleWriteComplete_callback(
        std::bind_front(&ClosingWriteCompletion::HandleWriteComplete,
                        &completion));
    c->Start();
    c->Send(Bytes("done"));
    loop.PollOnce(100);
    Expect(alive == 1 && destroyed == 1 && !c && ::fcntl(fd, F_GETFD) == -1 &&
               p.Collect() == "done",
           "drain callback deferred destruction preserves live object");
    std::cout << "drain destruction: callback_alive=" << alive
              << " returned=" << returned << " destroyed=" << destroyed << "\n";
  }
}

void BoundaryRejectionAndFin() {
  std::size_t rejected{}, truncated{};
  for (const auto& sample : protocol_cases::Rejections())
    for (bool prefix : {false, true}) {
      EventLoop loop;
      Pair pair;
      std::size_t calls{}, suffix_calls{}, closes{};
      TcpConnection c(loop, std::move(pair.owner), 81, http::kMaxRequestBytes);
      using ResponseScenario6State0 = decltype((calls));
      using ResponseScenario6State1 = decltype((suffix_calls));
      struct ResponseScenario6 {
        ResponseScenario6State0 calls;
        ResponseScenario6State1 suffix_calls;
        hp::http::ResponseResult PrepareResponse(
            const http::HttpRequest& r,
            http::ConnectionPolicy policy) {
          ++calls;
          if (r.target == "/suffix") ++suffix_calls;
          if (r.target == "/bad%20target")
            return http::ResponseResult{
                http::MakeErrorResponse(http::Status::kBadRequest),
                http::ConnectionPolicy::kClose};
          return http::ResponseResult{http::MakeResponse(http::Status::kOk,
                                                         Bytes(r.target),
                                                         "text/plain",
                                                         false,
                                                         policy),
                                      policy};
        }
      };
      c.set_HandleMessage_callback(app::MakeHttpCallback(
          std::bind_front(&ResponseScenario6::PrepareResponse,
                          ResponseScenario6{calls, suffix_calls})));
      using OnConnectionClosedObserver8State0 = decltype((closes));
      struct OnConnectionClosedObserver8 {
        OnConnectionClosedObserver8State0 closes;
        void OnConnectionClosed(int, TcpConnection::Identity) { ++closes; }
      };
      c.set_OnConnectionClosed_callback(
          std::bind_front(&OnConnectionClosedObserver8::OnConnectionClosed,
                          OnConnectionClosedObserver8{closes}));
      c.Start();
      pair.Send((prefix ? Request("/prefix") : "") + sample.wire +
                Request("/suffix"));
      const auto expected =
          (prefix ? Response("/prefix") : "") +
          Text(http::MakeErrorResponse(sample.status == 405
                                           ? http::Status::kMethodNotAllowed
                                           : http::Status::kBadRequest));
      Expect(Pump(loop, pair, c, expected.size()) == expected,
             "S3 component rejection ordered bytes");
      for (int i = 0; i < 5; ++i) loop.PollOnce(0);
      Expect(
          calls == static_cast<std::size_t>(prefix) + sample.reaches_service &&
              suffix_calls == 0 && closes == 1 && pair.Collect().empty(),
          "S3 rejection invokes no suffix and closes once");
      std::cout << "S3 component reject=" << sample.id
                << " position=" << (prefix ? 2 : 1) << " provider=" << calls
                << " suffix=" << suffix_calls << " closes=" << closes << '\n';
      ++rejected;
    }
  for (bool reused : {false, true})
    for (std::size_t cut = 1; cut < protocol_cases::kShortRequest.size();
         ++cut) {
      EventLoop loop;
      Pair pair;
      std::size_t calls{}, closes{};
      TcpConnection c(loop, std::move(pair.owner), 82, http::kMaxRequestBytes);
      using ResponseScenario7State0 = decltype((calls));
      struct ResponseScenario7 {
        ResponseScenario7State0 calls;
        hp::http::ResponseResult PrepareResponse(
            const http::HttpRequest& r,
            http::ConnectionPolicy policy) {
          ++calls;
          return http::ResponseResult{http::MakeResponse(http::Status::kOk,
                                                         Bytes(r.target),
                                                         "text/plain",
                                                         false,
                                                         policy),
                                      policy};
        }
      };
      c.set_HandleMessage_callback(app::MakeHttpCallback(
          std::bind_front(&ResponseScenario7::PrepareResponse,
                          ResponseScenario7{calls})));
      using OnConnectionClosedObserver9State0 = decltype((closes));
      struct OnConnectionClosedObserver9 {
        OnConnectionClosedObserver9State0 closes;
        void OnConnectionClosed(int, TcpConnection::Identity) { ++closes; }
      };
      c.set_OnConnectionClosed_callback(
          std::bind_front(&OnConnectionClosedObserver9::OnConnectionClosed,
                          OnConnectionClosedObserver9{closes}));
      c.Start();
      if (reused) {
        pair.Send(Request("/prefix"));
        Expect(Pump(loop, pair, c, Response("/prefix").size()) ==
                   Response("/prefix"),
               "successful prefix before truncation");
      }
      pair.Send(std::string_view(protocol_cases::kShortRequest).substr(0, cut));
      loop.PollOnce(100);
      Expect(pair.Collect().empty(), "incomplete prefix has no early response");
      Expect(::shutdown(pair.peer.fd(), SHUT_WR) == 0, "component FIN");
      const auto expected =
          Text(http::MakeErrorResponse(http::Status::kBadRequest));
      Expect(Pump(loop, pair, c, expected.size()) == expected,
             "truncated prefix produces exactly400");
      for (int i = 0; i < 5; ++i) loop.PollOnce(0);
      Expect(calls == static_cast<std::size_t>(reused) && closes == 1 &&
                 pair.Collect().empty(),
             "every FIN prefix closes once without provider");
      ++truncated;
    }
  std::cout << "S3 component rejection_cases=" << rejected
            << " FIN_prefix_cases=" << truncated
            << " duplicate_close=0 suffix_calls=0\n";
}

}  // namespace

int main() {
  try {
    PipelineSlowAndRepeated();
    TerminalMatrixAndEof();
    ServicePolicyAndFin();
    GenericDrains();
    BoundaryRejectionAndFin();
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
  std::cout << "keep_alive failures=" << failures << '\n';
  return failures ? 1 : 0;
}
