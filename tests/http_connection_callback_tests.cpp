#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <iostream>
#include <stdexcept>

#include "http/http_response.h"
#include "http_connection_handler.h"

namespace hp::net {
struct TcpConnectionTestAccess {
  static auto result(TcpConnection& c) { return c.last_result_; }

  static auto mask(TcpConnection& c) { return c.last_mask_; }

  static auto messages(TcpConnection& c) { return c.message_count_; }

  static auto buffered(TcpConnection& c) { return c.io_.input_view().size(); }

  static auto events(TcpConnection& c) { return c.event_count_; }

  static auto token(TcpConnection& c) { return c.connection_channel_.token(); }

  static auto interest(TcpConnection& c) {
    return c.connection_channel_.interest();
  }
};

struct TcpServerTestAccess {
  static auto& loop(TcpServer& s) { return s.loop_; }

  static void Add(TcpServer& s, Socket socket) {
    s.AddConnection(std::move(socket));
  }

  static auto& connection(TcpServer& s, int fd) {
    return *s.main_registry_->connections_.at(fd);
  }

  static auto size(TcpServer& s) {
    return s.main_registry_->connections_.size();
  }

  static void Notice(TcpServer& s, int fd, TcpConnection::Identity id) {
    s.main_registry_->OnConnectionClosed(fd, id);
  }

  static void Drain(TcpServer& s) {
    s.main_registry_->DrainClosedConnections();
  }
};

struct EventLoopTestAccess {
  static void Stale(EventLoop& loop, std::uint64_t token) {
    loop.Dispatch(token, EPOLLIN);
  }
};
}  // namespace hp::net

namespace {
using namespace hp;
using namespace hp::net;
using C = TcpConnectionTestAccess;
using S = TcpServerTestAccess;
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

std::size_t Fds() {
  return static_cast<std::size_t>(
      std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                    std::filesystem::directory_iterator{}));
}

struct Pair {
  Socket owner, peer;

  Pair() {
    int fd[2];
    if (::socketpair(AF_UNIX,
                     SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                     0,
                     fd))
      throw std::runtime_error("pair");
    owner.Reset(fd[0]);
    peer.Reset(fd[1]);
  }

  void Send(std::string_view s) {
    Expect(::send(peer.fd(), s.data(), s.size(), MSG_NOSIGNAL) ==
               static_cast<ssize_t>(s.size()),
           "send controlled input");
  }
};

std::vector<std::byte> Collect(int fd) {
  std::vector<std::byte> out;
  std::byte buffer[16384];
  ssize_t n;
  while ((n = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT)) > 0)
    out.insert(out.end(), buffer, buffer + n);
  return out;
}

std::vector<std::byte> Response(std::string_view body) {
  return http::MakeResponse(http::Status::kOk, Bytes(body), "text/plain");
}

const std::string kRequest =
    "GET /first HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n";

void InterleavedAndEof() {
  EventLoop loop;
  Pair a, b;
  app::HttpCallbackStats sa, sb;
  struct ResponseScenario1 {
    hp::http::ResponseResult PrepareResponse(const http::HttpRequest& r,
                                             http::ConnectionPolicy policy) {
      return http::ResponseResult{Response(r.target), policy};
    }
  };
  auto provider =
      std::bind_front(&ResponseScenario1::PrepareResponse, ResponseScenario1{});
  int closed{};
  TcpConnection ca(loop, std::move(a.owner), 1, http::kMaxRequestBytes);
  ca.set_HandleMessage_callback(app::MakeHttpCallback(provider, &sa));
  using OnConnectionClosedObserver1State0 = decltype((closed));
  struct OnConnectionClosedObserver1 {
    OnConnectionClosedObserver1State0 closed;
    void OnConnectionClosed(int, TcpConnection::Identity) { ++closed; }
  };
  ca.set_OnConnectionClosed_callback(
      std::bind_front(&OnConnectionClosedObserver1::OnConnectionClosed,
                      OnConnectionClosedObserver1{closed}));
  TcpConnection cb(loop, std::move(b.owner), 2, http::kMaxRequestBytes);
  cb.set_HandleMessage_callback(app::MakeHttpCallback(provider, &sb));
  using OnConnectionClosedObserver2State0 = decltype((closed));
  struct OnConnectionClosedObserver2 {
    OnConnectionClosedObserver2State0 closed;
    void OnConnectionClosed(int, TcpConnection::Identity) { ++closed; }
  };
  cb.set_OnConnectionClosed_callback(
      std::bind_front(&OnConnectionClosedObserver2::OnConnectionClosed,
                      OnConnectionClosedObserver2{closed}));
  ca.Start();
  cb.Start();
  a.Send("GET /one HTTP/1.1\r\n");
  loop.PollOnce(250);
  b.Send("GET /two HTTP/1.1\r\nHost: test\r\nConnection: close\r\n");
  loop.PollOnce(250);
  Expect(Collect(a.peer.fd()).empty() && Collect(b.peer.fd()).empty(),
         "independent NeedMore no response");
  a.Send("Host: test\r\nConnection: close\r\n\r\n");
  loop.PollOnce(250);
  b.Send("\r\n");
  loop.PollOnce(250);
  Expect(Collect(a.peer.fd()) == Response("/one") &&
             Collect(b.peer.fd()) == Response("/two"),
         "interleaved exact distinct responses");
  Expect(sa.responses == 1 && sb.responses == 1 && sa.need_more == 1 &&
             sb.need_more == 1 && closed == 2,
         "per-connection done state");
  Pair eof;
  app::HttpCallbackStats se;
  TcpConnection ce(loop, std::move(eof.owner), 3, http::kMaxRequestBytes);
  ce.set_HandleMessage_callback(app::MakeHttpCallback(provider, &se));
  struct OnConnectionClosedObserver3 {
    void OnConnectionClosed(int, TcpConnection::Identity) {}
  };
  ce.set_OnConnectionClosed_callback(
      std::bind_front(&OnConnectionClosedObserver3::OnConnectionClosed,
                      OnConnectionClosedObserver3{}));
  ce.Start();
  eof.Send("GET / HTTP/1.1\r\n");
  loop.PollOnce(250);
  ::shutdown(eof.peer.fd(), SHUT_WR);
  loop.PollOnce(250);
  Expect(Collect(eof.peer.fd()) ==
                 http::MakeErrorResponse(http::Status::kBadRequest) &&
             se.eof_notifications == 1,
         "EOF without new bytes incomplete400");
  int eof_empty{};
  Pair empty;
  TcpConnection cc(loop, std::move(empty.owner), 4, 0);
  using HandleMessageObserver4State0 = decltype((eof_empty));
  struct HandleMessageObserver4 {
    HandleMessageObserver4State0 eof_empty;
    void HandleMessage(TcpConnection& c,
                       std::span<const std::byte> in,
                       bool ended) {
      if (ended && in.empty()) ++eof_empty;
      c.CloseAfterFlush();
    }
  };
  cc.set_HandleMessage_callback(
      std::bind_front(&HandleMessageObserver4::HandleMessage,
                      HandleMessageObserver4{eof_empty}));
  struct OnConnectionClosedObserver5 {
    void OnConnectionClosed(int, TcpConnection::Identity) {}
  };
  cc.set_OnConnectionClosed_callback(
      std::bind_front(&OnConnectionClosedObserver5::OnConnectionClosed,
                      OnConnectionClosedObserver5{}));
  cc.Start();
  ::shutdown(empty.peer.fd(), SHUT_WR);
  loop.PollOnce(250);
  Expect(eof_empty == 1, "empty EOF notification once");
  std::cout << "isolation: callbacks=" << sa.callbacks + sb.callbacks
            << " need_more=" << sa.need_more + sb.need_more
            << " unique_responses=" << closed << " eof400=" << se.responses
            << " empty_eof=" << eof_empty << '\n';
}

void IncrementalConsumption() {
  EventLoop loop;
  Pair a, b;
  app::HttpCallbackStats sa, sb;
  struct ResponseScenario2 {
    hp::http::ResponseResult PrepareResponse(const http::HttpRequest& r,
                                             http::ConnectionPolicy policy) {
      return http::ResponseResult{Response(r.target), policy};
    }
  };
  auto provider =
      std::bind_front(&ResponseScenario2::PrepareResponse, ResponseScenario2{});
  TcpConnection ca(loop, std::move(a.owner), 90, http::kMaxRequestBytes);
  ca.set_HandleMessage_callback(app::MakeHttpCallback(provider, &sa));
  struct OnConnectionClosedObserver6 {
    void OnConnectionClosed(int, TcpConnection::Identity) {}
  };
  ca.set_OnConnectionClosed_callback(
      std::bind_front(&OnConnectionClosedObserver6::OnConnectionClosed,
                      OnConnectionClosedObserver6{}));
  TcpConnection cb(loop, std::move(b.owner), 91, http::kMaxRequestBytes);
  cb.set_HandleMessage_callback(app::MakeHttpCallback(provider, &sb));
  struct OnConnectionClosedObserver7 {
    void OnConnectionClosed(int, TcpConnection::Identity) {}
  };
  cb.set_OnConnectionClosed_callback(
      std::bind_front(&OnConnectionClosedObserver7::OnConnectionClosed,
                      OnConnectionClosedObserver7{}));
  ca.Start();
  cb.Start();
  const std::vector<std::string> pa = {"GET /",
                                       "a HTTP/1.1\r\nHo",
                                       "st: x\r\nConnection: close\r\n\r\n"};
  const std::vector<std::string> pb = {"GET /b HTTP/1.1\r",
                                       "\nHost: y\r",
                                       "\nConnection: close\r\n\r\n"};
  std::size_t sent_a = 0, sent_b = 0;
  for (std::size_t i = 0; i < pa.size(); ++i) {
    a.Send(pa[i]);
    sent_a += pa[i].size();
    loop.PollOnce(250);
    Expect(sa.parses == i + 1 && sa.accepted_bytes == sent_a &&
               sa.submitted_bytes == sent_a && sa.consumed_bytes == sent_a &&
               C::buffered(ca) == 0,
           "each A segment fed once and released before next write");
    b.Send(pb[i]);
    sent_b += pb[i].size();
    loop.PollOnce(250);
    Expect(sb.parses == i + 1 && sb.accepted_bytes == sent_b &&
               sb.submitted_bytes == sent_b && sb.consumed_bytes == sent_b &&
               C::buffered(cb) == 0,
           "each B segment fed once and released before next write");
    if (i < 2)
      Expect(sa.responses == 0 && sb.responses == 0,
             "incremental partial states independent");
  }
  Expect(Collect(a.peer.fd()) == Response("/a") &&
             Collect(b.peer.fd()) == Response("/b") && sa.need_more == 2 &&
             sb.need_more == 2,
         "three fragments preserve independent request fields");
  Pair empty;
  app::HttpCallbackStats se;
  TcpConnection ce(loop, std::move(empty.owner), 92, http::kMaxRequestBytes);
  ce.set_HandleMessage_callback(app::MakeHttpCallback(provider, &se));
  struct OnConnectionClosedObserver8 {
    void OnConnectionClosed(int, TcpConnection::Identity) {}
  };
  ce.set_OnConnectionClosed_callback(
      std::bind_front(&OnConnectionClosedObserver8::OnConnectionClosed,
                      OnConnectionClosedObserver8{}));
  ce.Start();
  ::shutdown(empty.peer.fd(), SHUT_WR);
  loop.PollOnce(250);
  Expect(Collect(empty.peer.fd()) ==
                 http::MakeErrorResponse(http::Status::kBadRequest) &&
             se.accepted_bytes == 0 && se.eof_notifications == 1,
         "empty HTTP EOF feed produces400 without consumed bytes");
  std::cout
      << "incremental: feeds=" << sa.parses + sb.parses
      << " need_more=" << sa.need_more + sb.need_more
      << " submitted=" << sa.submitted_bytes + sb.submitted_bytes
      << " accepted=" << sa.accepted_bytes + sb.accepted_bytes
      << " consumed=" << sa.consumed_bytes + sb.consumed_bytes
      << " network_buffer=0 independent_responses=2 empty_http_eof400=1\n";
}

void LimitsAnd500() {
  EventLoop loop;
  struct ResponseScenario3 {
    hp::http::ResponseResult PrepareResponse(const http::HttpRequest&,
                                             http::ConnectionPolicy policy) {
      return http::ResponseResult{Response("ok"), policy};
    }
  };
  auto provider =
      std::bind_front(&ResponseScenario3::PrepareResponse, ResponseScenario3{});
  std::string exact = "GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\nX: ";
  exact.append(http::kMaxRequestBytes - exact.size() - 4, 'a');
  exact += "\r\n\r\n";
  for (bool over : {false, true}) {
    Pair p;
    app::HttpCallbackStats st;
    TcpConnection c(loop, std::move(p.owner), 5, http::kMaxRequestBytes);
    c.set_HandleMessage_callback(app::MakeHttpCallback(provider, &st));
    struct OnConnectionClosedObserver9 {
      void OnConnectionClosed(int, TcpConnection::Identity) {}
    };
    c.set_OnConnectionClosed_callback(
        std::bind_front(&OnConnectionClosedObserver9::OnConnectionClosed,
                        OnConnectionClosedObserver9{}));
    c.Start();
    std::string input =
        over ? std::string(http::kMaxRequestBytes + 1, 'a') : exact;
    p.Send(input);
    loop.PollOnce(250);
    auto got = Collect(p.peer.fd());
    Expect(got == (over ? http::MakeErrorResponse(http::Status::kBadRequest)
                        : Response("ok")),
           "precise HTTP input bound response");
  }
  Pair p;
  app::HttpCallbackStats st;
  TcpConnection c(loop, std::move(p.owner), 6, http::kMaxRequestBytes);
  struct ResponseScenario4 {
    hp::http::ResponseResult PrepareResponse(const http::HttpRequest&,
                                             http::ConnectionPolicy) {
      throw std::runtime_error("provider");
    }
  };
  c.set_HandleMessage_callback(app::MakeHttpCallback(
      std::bind_front(&ResponseScenario4::PrepareResponse, ResponseScenario4{}),
      &st));
  struct OnConnectionClosedObserver10 {
    void OnConnectionClosed(int, TcpConnection::Identity) {}
  };
  c.set_OnConnectionClosed_callback(
      std::bind_front(&OnConnectionClosedObserver10::OnConnectionClosed,
                      OnConnectionClosedObserver10{}));
  c.Start();
  p.Send(kRequest);
  loop.PollOnce(250);
  Expect(Collect(p.peer.fd()) ==
                 http::MakeErrorResponse(http::Status::kInternalServerError) &&
             st.responses == 1,
         "adapter exception maps500");
  int exact_seen{}, limit_closed{};
  Pair bounded;
  TcpConnection cap(loop, std::move(bounded.owner), 7, 4);
  using HandleMessageObserver11State0 = decltype((exact_seen));
  struct HandleMessageObserver11 {
    HandleMessageObserver11State0 exact_seen;
    void HandleMessage(TcpConnection&, std::span<const std::byte> in, bool) {
      if (in.size() == 4) ++exact_seen;
    }
  };
  cap.set_HandleMessage_callback(
      std::bind_front(&HandleMessageObserver11::HandleMessage,
                      HandleMessageObserver11{exact_seen}));
  using OnConnectionClosedObserver12State0 = decltype((limit_closed));
  struct OnConnectionClosedObserver12 {
    OnConnectionClosedObserver12State0 limit_closed;
    void OnConnectionClosed(int, TcpConnection::Identity) { ++limit_closed; }
  };
  cap.set_OnConnectionClosed_callback(
      std::bind_front(&OnConnectionClosedObserver12::OnConnectionClosed,
                      OnConnectionClosedObserver12{limit_closed}));
  cap.Start();
  bounded.Send("12345");
  loop.PollOnce(250);
  Expect(exact_seen == 1 && limit_closed == 1 &&
             C::result(cap).read_error == EMSGSIZE,
         "notify exact cap before controlled overflow close");
  std::cout << "limits: exact_http=1 over_http=1 exact_cap_notification="
            << exact_seen << " cap_close=" << limit_closed
            << " adapter500=" << st.responses << '\n';
}

void DrainPipelineAndBorrow() {
  EventLoop loop;
  Pair p;
  int sndbuf = 4096;
  ::setsockopt(p.owner.fd(), SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
  app::HttpCallbackStats stats;
  std::vector<std::byte> body(512 * 1024);
  for (std::size_t i = 0; i < body.size(); ++i)
    body[i] = static_cast<std::byte>(i * 71U);
  const auto expected =
      http::MakeResponse(http::Status::kOk, body, "application/octet-stream");
  int closes{};
  TcpConnection c(loop, std::move(p.owner), 8, http::kMaxRequestBytes);
  using ResponseScenario5State0 = decltype((body));
  struct ResponseScenario5 {
    ResponseScenario5State0 body;
    hp::http::ResponseResult PrepareResponse(const http::HttpRequest&,
                                             http::ConnectionPolicy policy) {
      return http::ResponseResult{http::MakeResponse(http::Status::kOk,
                                                     body,
                                                     "application/octet-stream",
                                                     false,
                                                     policy),
                                  policy};
    }
  };
  c.set_HandleMessage_callback(
      app::MakeHttpCallback(std::bind_front(&ResponseScenario5::PrepareResponse,
                                            ResponseScenario5{body}),
                            &stats));
  using OnConnectionClosedObserver13State0 = decltype((closes));
  struct OnConnectionClosedObserver13 {
    OnConnectionClosedObserver13State0 closes;
    void OnConnectionClosed(int, TcpConnection::Identity) { ++closes; }
  };
  c.set_OnConnectionClosed_callback(
      std::bind_front(&OnConnectionClosedObserver13::OnConnectionClosed,
                      OnConnectionClosedObserver13{closes}));
  c.Start();
  p.Send(kRequest + kRequest);
  loop.PollOnce(250);
  const auto pending = c.pending_bytes();
  Expect(pending > 0 && C::result(c).write_would_block && closes == 0,
         "CloseAfterFlush retains temporary response tail");
  const auto parses = stats.parses, sends = stats.responses,
             callbacks = stats.callbacks;
  p.Send(kRequest);
  ::shutdown(p.peer.fd(), SHUT_WR);
  std::vector<std::byte> received;
  for (int i = 0; i < 1000 && received.size() < expected.size(); ++i) {
    auto part = Collect(p.peer.fd());
    received.insert(received.end(), part.begin(), part.end());
    loop.PollOnce(0);
  }
  auto part = Collect(p.peer.fd());
  received.insert(received.end(), part.begin(), part.end());
  Expect(received == expected && c.pending_bytes() == 0 && closes == 1 &&
             !(C::interest(c) & EPOLLOUT),
         "owned temporary bytes drain exactly before close");
  const auto events = C::events(c);
  for (int i = 0; i < 10; ++i) loop.PollOnce(0);
  Expect(stats.parses == parses && stats.responses == sends &&
             stats.callbacks == callbacks && C::events(c) == events,
         "same/later pipeline and EOF no second parse/send or busyloop");
  Pair invalid;
  int consumed_close{};
  TcpConnection bad(loop, std::move(invalid.owner), 9, 0);
  struct HandleMessageObserver14 {
    void HandleMessage(TcpConnection& c,
                       std::span<const std::byte> input,
                       bool) {
      c.Consume(input.size() + 1);
    }
  };
  bad.set_HandleMessage_callback(
      std::bind_front(&HandleMessageObserver14::HandleMessage,
                      HandleMessageObserver14{}));
  using OnConnectionClosedObserver15State0 = decltype((consumed_close));
  struct OnConnectionClosedObserver15 {
    OnConnectionClosedObserver15State0 consumed_close;
    void OnConnectionClosed(int, TcpConnection::Identity) { ++consumed_close; }
  };
  bad.set_OnConnectionClosed_callback(
      std::bind_front(&OnConnectionClosedObserver15::OnConnectionClosed,
                      OnConnectionClosedObserver15{consumed_close}));
  bad.Start();
  invalid.Send("x");
  loop.PollOnce(250);
  Expect(consumed_close == 1, "consume overflow contained");
  std::cout << "drain: pending=" << pending
            << " eagain=1 full_bytes=" << received.size()
            << " final_pending=" << c.pending_bytes() << " close=" << closes
            << " second_parse=" << stats.parses - parses
            << " second_send=" << stats.responses - sends
            << " empty_polls=10 consume_error=" << consumed_close << '\n';
}

void FactoryMessageLifetime() {
  const auto before = Fds();
  int factory_errors{}, message_errors{}, callback_alive{}, recovered{};
  {
    int factories{};
    TcpServer server(0);
    using FailureCallbacksState0 = decltype((factories));
    using FailureCallbacksState1 = decltype((factory_errors));
    using FailureCallbacksState2 = decltype((message_errors));
    using FailureCallbacksState3 = decltype((callback_alive));
    using FailureCallbacksState4 = decltype((recovered));
    struct FailureCallbacks {
      FailureCallbacksState0 factories;
      FailureCallbacksState1 factory_errors;
      FailureCallbacksState2 message_errors;
      FailureCallbacksState3 callback_alive;
      FailureCallbacksState4 recovered;
      void HandleMessage(TcpConnection& c,
                         std::span<const std::byte> input,
                         bool) {
        if (input[0] == std::byte{'x'}) {
          ++message_errors;
          throw std::runtime_error("message");
        }
        if (input[0] == std::byte{'c'}) {
          c.RequestClose();
          c.RequestClose();
          if (::fcntl(c.fd(), F_GETFD) >= 0) ++callback_alive;
          return;
        }
        ++recovered;
        c.Send(input);
        c.Consume(input.size());
      }
      TcpConnection::MessageCallback CreateMessageCallback() {
        if (++factories == 1) {
          ++factory_errors;
          throw std::runtime_error("factory");
        }
        return std::bind_front(&FailureCallbacks::HandleMessage, *this);
      }
    };
    server.set_CreateMessageCallback_callback(
        std::bind_front(&FailureCallbacks::CreateMessageCallback,
                        FailureCallbacks{factories,
                                         factory_errors,
                                         message_errors,
                                         callback_alive,
                                         recovered}));
    Pair first;
    int ff = first.owner.fd();
    try {
      S::Add(server, std::move(first.owner));
    } catch (const std::runtime_error&) {
    }
    Expect(S::size(server) == 0 && ::fcntl(ff, F_GETFD) == -1,
           "factory failure owns/closes socket");
    Pair second;
    int oldfd = second.owner.fd();
    S::Add(server, std::move(second.owner));
    auto& old = S::connection(server, oldfd);
    auto oldtoken = C::token(old), oldid = old.identity();
    second.Send("x");
    S::loop(server).PollOnce(250);
    Expect(S::size(server) == 0, "message exception closes one");
    Pair fresh;
    if (fresh.owner.fd() != oldfd) {
      Expect(::dup2(fresh.owner.fd(), oldfd) == oldfd, "reuse numeric fd");
      fresh.owner.Reset(oldfd);
    }
    S::Add(server, std::move(fresh.owner));
    EventLoopTestAccess::Stale(S::loop(server), oldtoken);
    S::Notice(server, oldfd, oldid);
    S::Drain(server);
    Expect(
        S::size(server) == 1 && C::messages(S::connection(server, oldfd)) == 0,
        "old close/token no misdelete");
    fresh.Send("ok");
    S::loop(server).PollOnce(250);
    Expect(Collect(fresh.peer.fd()) ==
               std::vector<std::byte>(Bytes("ok").begin(), Bytes("ok").end()),
           "new callback recovers echo");
    fresh.Send("c");
    S::loop(server).PollOnce(250);
    Expect(S::size(server) == 0 && ::fcntl(oldfd, F_GETFD) == -1,
           "callback returns before fd close");
    Pair output;
    TcpConnection c(S::loop(server), std::move(output.owner), 77, 0);
    struct OnConnectionClosedObserver16 {
      void OnConnectionClosed(int, TcpConnection::Identity) {}
    };
    c.set_OnConnectionClosed_callback(
        std::bind_front(&OnConnectionClosedObserver16::OnConnectionClosed,
                        OnConnectionClosedObserver16{}));
    c.Start();
    output.peer.Reset();
    c.Send(Bytes("out"));
    Expect(c.state() == TcpConnection::State::kClosing,
           "send output error closes");
    Pair active;
    S::Add(server, std::move(active.owner));  // destructor with live owner
    std::cout << "identity: stale=" << S::loop(server).counters().stale
              << " old_close_misdelete=0 new_callback=" << recovered << '\n';
  }
  Expect(Fds() == before && factory_errors == 1 && message_errors == 1 &&
             callback_alive == 1 && recovered == 1,
         "failure lifecycle and fd restoration");
  std::cout << "failures: factory=" << factory_errors
            << " message=" << message_errors
            << " callback_alive=" << callback_alive << " recovery=" << recovered
            << " output_error=1 fd=" << before << "->" << Fds() << '\n';
}

void ResetNewMessage() {
  EventLoop loop;
  Socket accepted;
  Acceptor acceptor(loop, 0);
  using AddConnectionObserver17State0 = decltype((accepted));
  struct AddConnectionObserver17 {
    AddConnectionObserver17State0 accepted;
    void AddConnection(Socket s) { accepted = std::move(s); }
  };
  acceptor.set_AddConnection_callback(
      std::bind_front(&AddConnectionObserver17::AddConnection,
                      AddConnectionObserver17{accepted}));
  acceptor.Start();
  Socket client(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(acceptor.bound_port());
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  Expect(::connect(client.fd(),
                   reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) == 0,
         "reset TCP connect");
  loop.PollOnce(250);
  acceptor.Stop();
  std::vector<std::byte> received;
  int messages{}, closed{};
  TcpConnection c(loop, std::move(accepted), 78, 0);
  using HandleMessageObserver18State0 = decltype((messages));
  using HandleMessageObserver18State1 = decltype((received));
  struct HandleMessageObserver18 {
    HandleMessageObserver18State0 messages;
    HandleMessageObserver18State1 received;
    void HandleMessage(TcpConnection& conn,
                       std::span<const std::byte> input,
                       bool) {
      ++messages;
      received.insert(received.end(), input.begin(), input.end());
      conn.Consume(input.size());
    }
  };
  c.set_HandleMessage_callback(
      std::bind_front(&HandleMessageObserver18::HandleMessage,
                      HandleMessageObserver18{messages, received}));
  using OnConnectionClosedObserver19State0 = decltype((closed));
  struct OnConnectionClosedObserver19 {
    OnConnectionClosedObserver19State0 closed;
    void OnConnectionClosed(int, TcpConnection::Identity) { ++closed; }
  };
  c.set_OnConnectionClosed_callback(
      std::bind_front(&OnConnectionClosedObserver19::OnConnectionClosed,
                      OnConnectionClosedObserver19{closed}));
  c.Start();
  std::vector<std::byte> queued(1053, std::byte{0x5a});
  Expect(
      ::send(client.fd(), queued.data(), queued.size(), MSG_NOSIGNAL) == 1053,
      "reset queue bytes");
  std::vector<std::byte> peek(1053);
  ssize_t count = -1;
  for (int i = 0; i < 1000; ++i) {
    count = ::recv(c.fd(), peek.data(), peek.size(), MSG_PEEK);
    if (count == 1053) break;
    ::usleep(1000);
  }
  Expect(count == 1053 && peek == queued, "reset full queued");
  Epoller waiting;
  waiting.Add(c.fd(), 0, 1);
  linger reset{1, 0};
  ::setsockopt(client.fd(), SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
  client.Reset();
  bool pending = false;
  for (int i = 0; i < 8 && !pending; ++i) {
    for (const auto& e : waiting.Wait(250))
      if (e.events & EPOLLERR) pending = true;
  }
  Expect(pending, "reset error ready");
  waiting.Remove(c.fd());
  loop.PollOnce(250);
  auto result = C::result(c);
  bool combined = (C::mask(c) & (EPOLLERR | EPOLLIN)) == (EPOLLERR | EPOLLIN);
  Expect(combined && result.socket_error == ECONNRESET &&
             result.bytes_read == 1053 && received == queued && messages > 0 &&
             closed == 1,
         "new callback receives all diagnosed reset bytes");
  std::cout << "reset: combined=" << combined
            << " so_error=" << result.socket_error
            << " recv_bytes=" << result.bytes_read
            << " message_bytes=" << received.size() << " messages=" << messages
            << " closes=" << closed << '\n';
}
}  // namespace

int main() {
  InterleavedAndEof();
  IncrementalConsumption();
  LimitsAnd500();
  DrainPipelineAndBorrow();
  FactoryMessageLifetime();
  ResetNewMessage();
  std::cout << "HTTP callback assertions_failed=" << failures << '\n';
  return failures ? 1 : 0;
}
