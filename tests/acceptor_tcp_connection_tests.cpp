#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>

#include "net/tcp_server.h"

namespace hp::net {
struct EventLoopTestAccess {
  static void Stale(EventLoop& loop, std::uint64_t token) {
    loop.Dispatch(token, EPOLLIN);
  }

  static void DispatchAfterKernelDetach(EventLoop& loop, int fd) {
    // Keep a real queued event, detach only kernel registration, then dispatch
    // through production Channel/TcpConnection so the next MOD fails ENOENT.
    const auto events = loop.epoller_.Wait(250);
    if (events.size() != 1)
      throw std::runtime_error("expected one pending event");
    const auto event = events.front();
    loop.epoller_.Remove(fd);
    loop.Dispatch(event.data.u64, event.events);
  }
};

struct TcpConnectionTestAccess {
  static auto token(TcpConnection& c) { return c.connection_channel_.token(); }

  static auto events(TcpConnection& c) { return c.event_count_; }

  static auto pending(TcpConnection& c) { return c.io_.pending_bytes(); }

  static auto interest(TcpConnection& c) {
    return c.connection_channel_.interest();
  }

  static auto result(TcpConnection& c) { return c.last_result_; }

  static auto mask(TcpConnection& c) { return c.last_mask_; }

  static void set_interest(TcpConnection& c, std::uint32_t mask) {
    c.connection_channel_.set_interest(mask);
  }
};

struct AcceptorTestAccess {
  static int fd(Acceptor& a) { return a.listener_.fd(); }

  static void InvalidateListener(Acceptor& a) { a.listener_.Reset(); }
};

struct TcpServerTestAccess {
  static EventLoop& loop(TcpServer& s) { return s.loop_; }

  static void StartAccepting(TcpServer& server) { server.acceptor_.Start(); }

  static void Add(TcpServer& s, Socket socket) {
    s.AddConnection(std::move(socket));
  }

  static TcpConnection& connection(TcpServer& s, int fd) {
    return *s.main_registry_->connections_.at(fd);
  }

  static auto size(TcpServer& s) {
    return s.main_registry_->connections_.size();
  }

  static void CloseNotice(TcpServer& s, int fd, TcpConnection::Identity id) {
    s.main_registry_->OnConnectionClosed(fd, id);
  }

  static void Drain(TcpServer& s) {
    s.main_registry_->DrainClosedConnections();
  }
};
}  // namespace hp::net

namespace {
using namespace hp::net;
using C = TcpConnectionTestAccess;
using S = TcpServerTestAccess;
int failures{};

void Expect(bool ok, const char* text) {
  if (!ok) {
    ++failures;
    std::cerr << "FAIL: " << text << '\n';
  }
}

std::size_t FdCount() {
  return static_cast<std::size_t>(
      std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                    std::filesystem::directory_iterator{}));
}

struct Pair {
  Socket observed, peer;

  Pair() {
    int fds[2];
    if (::socketpair(AF_UNIX,
                     SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                     0,
                     fds))
      throw std::runtime_error("socketpair");
    observed.Reset(fds[0]);
    peer.Reset(fds[1]);
  }

  void Send(std::span<const std::byte> bytes) {
    Expect(::send(peer.fd(), bytes.data(), bytes.size(), MSG_NOSIGNAL) ==
               static_cast<ssize_t>(bytes.size()),
           "send controlled bytes");
  }

  void SendReadiness() {
    const std::byte b{0x61};
    Send({&b, 1});
  }
};

Socket ConnectTo(std::uint16_t port) {
  Socket client(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (!client.valid() || ::connect(client.fd(),
                                   reinterpret_cast<sockaddr*>(&address),
                                   sizeof(address)))
    throw std::runtime_error("connect");
  return client;
}

void AwaitAcceptQueue(Acceptor& acceptor, unsigned expected) {
  // Linux reports the listener accept backlog in TCP_INFO.tcpi_unacked.
  // Read the kernel queue before the single dispatch, rather than assuming that
  // client connect() or a fixed delay means the server processed the final ACK.
  for (int attempt = 0; attempt < 1000; ++attempt) {
    tcp_info info{};
    socklen_t length = sizeof(info);
    if (::getsockopt(AcceptorTestAccess::fd(acceptor),
                     IPPROTO_TCP,
                     TCP_INFO,
                     &info,
                     &length))
      throw std::runtime_error("listener TCP_INFO");
    if (info.tcpi_unacked == expected) return;
    ::usleep(1000);
  }
  throw std::runtime_error(
      "listener accept queue did not reach expected count");
}

void AcceptorDelivery() {
  const auto before = FdCount();
  int accepted{}, transferred{}, failures_seen{}, callbacks_after_stop{},
      bind_failure{}, initial_drain{};
  {
    EventLoop loop;
    std::vector<Socket> owners, clients;
    std::set<int> seen;
    bool stopped = false, fail_delivery = false;
    Acceptor acceptor(loop, 0);
    using AddConnectionObserver1State0 = decltype((accepted));
    using AddConnectionObserver1State1 = decltype((stopped));
    using AddConnectionObserver1State2 = decltype((callbacks_after_stop));
    using AddConnectionObserver1State3 = decltype((fail_delivery));
    using AddConnectionObserver1State4 = decltype((failures_seen));
    using AddConnectionObserver1State5 = decltype((seen));
    using AddConnectionObserver1State6 = decltype((owners));
    using AddConnectionObserver1State7 = decltype((transferred));
    struct AddConnectionObserver1 {
      AddConnectionObserver1State0 accepted;
      AddConnectionObserver1State1 stopped;
      AddConnectionObserver1State2 callbacks_after_stop;
      AddConnectionObserver1State3 fail_delivery;
      AddConnectionObserver1State4 failures_seen;
      AddConnectionObserver1State5 seen;
      AddConnectionObserver1State6 owners;
      AddConnectionObserver1State7 transferred;
      void AddConnection(Socket socket) {
        ++accepted;
        if (stopped) ++callbacks_after_stop;
        Expect((::fcntl(socket.fd(), F_GETFL) & O_NONBLOCK) != 0,
               "accepted nonblocking");
        Expect((::fcntl(socket.fd(), F_GETFD) & FD_CLOEXEC) != 0,
               "accepted CLOEXEC");
        if (fail_delivery) {
          ++failures_seen;
          fail_delivery = false;
          throw std::runtime_error("delivery failure");
        }
        Expect(seen.insert(socket.fd()).second, "no duplicate delivery");
        owners.push_back(std::move(socket));
        ++transferred;
      }
    };
    acceptor.set_AddConnection_callback(
        std::bind_front(&AddConnectionObserver1::AddConnection,
                        AddConnectionObserver1{accepted,
                                               stopped,
                                               callbacks_after_stop,
                                               fail_delivery,
                                               failures_seen,
                                               seen,
                                               owners,
                                               transferred}));
    acceptor.Start();
    acceptor.Start();
    for (int i = 0; i < 8; ++i)
      clients.push_back(ConnectTo(acceptor.bound_port()));
    AwaitAcceptQueue(acceptor, 8);
    const auto dispatch_before = loop.counters().dispatches;
    loop.PollOnce(250);
    initial_drain = accepted;
    AwaitAcceptQueue(acceptor, 0);
    Expect(accepted == 8 && transferred == 8 &&
               loop.counters().dispatches == dispatch_before + 1,
           "one listener callback drains eight queued clients");
    try {
      Acceptor conflict(loop, acceptor.bound_port());
      struct AddConnectionObserver2 {
        void AddConnection(Socket) {}
      };
      conflict.set_AddConnection_callback(
          std::bind_front(&AddConnectionObserver2::AddConnection,
                          AddConnectionObserver2{}));
    } catch (const std::system_error&) {
      ++bind_failure;
    }
    fail_delivery = true;
    clients.push_back(ConnectTo(acceptor.bound_port()));
    clients.push_back(ConnectTo(acceptor.bound_port()));
    AwaitAcceptQueue(acceptor, 2);
    const auto fd_before_failure = FdCount();
    loop.PollOnce(250);
    Expect(failures_seen == 1 && transferred == 9 &&
               FdCount() == fd_before_failure + 1,
           "failed delivery closes socket and next delivery succeeds");
    acceptor.Stop();
    acceptor.Stop();
    stopped = true;
    clients.push_back(ConnectTo(acceptor.bound_port()));
    loop.PollOnce(0);
    Expect(accepted == 10 && callbacks_after_stop == 0 && bind_failure == 1,
           "stop and bind failure");
  }
  Expect(FdCount() == before, "acceptor resources restored");
  std::cout << "acceptor: initial_drain=" << initial_drain
            << " accepted=" << accepted << " transferred=" << transferred
            << " delivery_failure=" << failures_seen
            << " bind_failure=" << bind_failure
            << " post_stop=" << callbacks_after_stop
            << " duplicate=0 fd=" << before << "->" << FdCount() << '\n';
}

void BuffersAndEcho() {
  EventLoop loop;
  Pair pair;
  int size = 4096;
  Expect(::setsockopt(pair.observed.fd(),
                      SOL_SOCKET,
                      SO_SNDBUF,
                      &size,
                      sizeof(size)) == 0,
         "small send buffer");
  int closes{};
  TcpConnection c(loop, std::move(pair.observed), 1, 0);
  using OnConnectionClosedObserver3State0 = decltype((closes));
  struct OnConnectionClosedObserver3 {
    OnConnectionClosedObserver3State0 closes;
    void OnConnectionClosed(int, TcpConnection::Identity) { ++closes; }
  };
  c.set_OnConnectionClosed_callback(
      std::bind_front(&OnConnectionClosedObserver3::OnConnectionClosed,
                      OnConnectionClosedObserver3{closes}));
  c.Start();
  c.Start();
  std::vector<std::byte> payload(65536);
  for (std::size_t i = 0; i < payload.size(); ++i)
    payload[i] = static_cast<std::byte>(i * 131U + 17U);
  pair.Send(payload);
  loop.PollOnce(250);
  const auto pending = C::pending(c);
  Expect(C::result(c).write_would_block && pending > 0 &&
             pending < payload.size() && (C::interest(c) & EPOLLOUT),
         "real partial send and EAGAIN preserve tail");
  std::vector<std::byte> received;
  std::byte buffer[8192];
  int iterations{};
  while (received.size() < payload.size() && iterations++ < 200) {
    ssize_t n;
    while ((n = ::recv(pair.peer.fd(), buffer, sizeof(buffer), 0)) > 0)
      received.insert(received.end(), buffer, buffer + n);
    loop.PollOnce(0);
  }
  Expect(
      received == payload && C::pending(c) == 0 && !(C::interest(c) & EPOLLOUT),
      "exact echoed bytes and disable write");
  const auto events = C::events(c);
  for (int i = 0; i < 10; ++i) loop.PollOnce(0);
  Expect(C::events(c) == events && closes == 0,
         "no writable busy-loop after drain");
  c.RequestClose();
  c.RequestClose();
  Expect(closes == 1, "one close notification");
  std::cout << "buffers: initial_pending=" << pending
            << " eagain=1 recovered_bytes=" << received.size()
            << " final_pending=" << C::pending(c) << " callbacks=" << events
            << " empty_polls=10 closes=" << closes << '\n';
}

void ApplicationBoundaries() {
  EventLoop loop;
  int calls{}, notices{};
  Pair pair;
  TcpConnection c(loop, std::move(pair.observed), 2, 2);
  using HandleMessageObserver4State0 = decltype((calls));
  struct HandleMessageObserver4 {
    HandleMessageObserver4State0 calls;
    void HandleMessage(TcpConnection& connection,
                       std::span<const std::byte> input,
                       bool) {
      ++calls;
      if (input.size() < 2) return;
      Expect(input.size() == 2, "accumulated borrowed input span");
      const std::byte response[]{std::byte{0x78}, std::byte{0x79}};
      connection.Send(response);
      connection.Consume(input.size());
      connection.CloseAfterFlush();
    }
  };
  c.set_HandleMessage_callback(
      std::bind_front(&HandleMessageObserver4::HandleMessage,
                      HandleMessageObserver4{calls}));
  using OnConnectionClosedObserver5State0 = decltype((notices));
  struct OnConnectionClosedObserver5 {
    OnConnectionClosedObserver5State0 notices;
    void OnConnectionClosed(int, TcpConnection::Identity) { ++notices; }
  };
  c.set_OnConnectionClosed_callback(
      std::bind_front(&OnConnectionClosedObserver5::OnConnectionClosed,
                      OnConnectionClosedObserver5{notices}));
  c.Start();
  pair.SendReadiness();
  loop.PollOnce(250);
  Expect(calls == 1 && c.state() == TcpConnection::State::kActive,
         "partial input no response");
  pair.SendReadiness();
  loop.PollOnce(250);
  std::byte bytes[8];
  const auto n = ::recv(pair.peer.fd(), bytes, sizeof(bytes), 0);
  Expect(n == 2 && bytes[0] == std::byte{0x78} && bytes[1] == std::byte{0x79} &&
             calls == 2 && notices == 1,
         "one copied response after local application result lifetime");
  pair.SendReadiness();
  loop.PollOnce(0);
  Expect(calls == 2, "removed connection does not call application again");
  Pair limit;
  int limit_calls{}, limit_closed{};
  TcpConnection capped(loop, std::move(limit.observed), 3, 2);
  using HandleMessageObserver6State0 = decltype((limit_calls));
  struct HandleMessageObserver6 {
    HandleMessageObserver6State0 limit_calls;
    void HandleMessage(TcpConnection& connection,
                       std::span<const std::byte> input,
                       bool) {
      (void)connection;
      ++limit_calls;
      Expect(input.size() <= 2, "input limit respected");
    }
  };
  capped.set_HandleMessage_callback(
      std::bind_front(&HandleMessageObserver6::HandleMessage,
                      HandleMessageObserver6{limit_calls}));
  using OnConnectionClosedObserver7State0 = decltype((limit_closed));
  struct OnConnectionClosedObserver7 {
    OnConnectionClosedObserver7State0 limit_closed;
    void OnConnectionClosed(int, TcpConnection::Identity) { ++limit_closed; }
  };
  capped.set_OnConnectionClosed_callback(
      std::bind_front(&OnConnectionClosedObserver7::OnConnectionClosed,
                      OnConnectionClosedObserver7{limit_closed}));
  capped.Start();
  std::byte over[3]{};
  limit.Send(over);
  loop.PollOnce(250);
  Expect(limit_closed == 1 && C::result(capped).read_error == EMSGSIZE,
         "oversized input closes within original cap");
  std::cout << "application: segmented_calls=" << calls
            << " one_response=1 limit_calls=" << limit_calls
            << " limit_close=" << limit_closed << '\n';
}

void CloseBatchAndReuse() {
  const auto before = FdCount();
  int in_callback_fd_alive{}, survivor_calls{}, self_calls{};
  std::size_t stale{};
  {
    TcpConnection* a{};
    TcpConnection* b{};
    TcpServer server(0);
    using CloseBatchCallbacksState0 = decltype((a));
    using CloseBatchCallbacksState1 = decltype((b));
    using CloseBatchCallbacksState2 = decltype((survivor_calls));
    using CloseBatchCallbacksState3 = decltype((self_calls));
    using CloseBatchCallbacksState4 = decltype((in_callback_fd_alive));
    struct CloseBatchCallbacks {
      CloseBatchCallbacksState0 a;
      CloseBatchCallbacksState1 b;
      CloseBatchCallbacksState2 survivor_calls;
      CloseBatchCallbacksState3 self_calls;
      CloseBatchCallbacksState4 in_callback_fd_alive;
      void HandleMessage(TcpConnection&,
                         std::span<const std::byte> bytes,
                         bool) {
        if (bytes[0] == std::byte{0x73}) {
          ++survivor_calls;
          return;
        }
        ++self_calls;
        a->RequestClose();
        a->RequestClose();
        b->RequestClose();
        if (::fcntl(a->fd(), F_GETFD) >= 0 && ::fcntl(b->fd(), F_GETFD) >= 0)
          ++in_callback_fd_alive;
        return;
      }
      TcpConnection::MessageCallback CreateMessageCallback() {
        return std::bind_front(&CloseBatchCallbacks::HandleMessage, *this);
      }
    };
    server.set_CreateMessageCallback_callback(
        std::bind_front(&CloseBatchCallbacks::CreateMessageCallback,
                        CloseBatchCallbacks{a,
                                            b,
                                            survivor_calls,
                                            self_calls,
                                            in_callback_fd_alive}));
    Pair x, y, z;
    int fx = x.observed.fd(), fy = y.observed.fd(), fz = z.observed.fd();
    S::Add(server, std::move(x.observed));
    S::Add(server, std::move(y.observed));
    S::Add(server, std::move(z.observed));
    a = &S::connection(server, fx);
    b = &S::connection(server, fy);
    x.SendReadiness();
    y.SendReadiness();
    std::byte marker{0x73};
    z.Send({&marker, 1});
    S::loop(server).PollOnce(250);
    stale = S::loop(server).counters().stale;
    Expect(self_calls == 1 && survivor_calls == 1 && stale == 1 &&
               S::size(server) == 1,
           "same batch two closes, survivor and stale");
    Expect(::fcntl(fx, F_GETFD) == -1 && ::fcntl(fy, F_GETFD) == -1 &&
               in_callback_fd_alive == 1,
           "close only after callback returns");
    const auto survivor_events = C::events(S::connection(server, fz));
    S::loop(server).PollOnce(0);
    Expect(C::events(S::connection(server, fz)) == survivor_events,
           "post remove no extra event");
    // Active survivor intentionally remains for server destructor cleanup.
  }
  Expect(FdCount() == before,
         "server destructor restores active connection resources");
  {
    TcpServer server(0);
    Pair old;
    const int fd = old.observed.fd();
    S::Add(server, std::move(old.observed));
    auto& original = S::connection(server, fd);
    const auto old_token = C::token(original), old_id = original.identity();
    original.RequestClose();
    S::Drain(server);
    Pair fresh;
    if (fresh.observed.fd() != fd) {
      Expect(::dup2(fresh.observed.fd(), fd) == fd, "force same numeric fd");
      fresh.observed.Reset(fd);
    }
    S::Add(server, std::move(fresh.observed));
    auto& current = S::connection(server, fd);
    Expect(current.identity() != old_id && C::token(current) != old_token,
           "new identity and token for reused fd");
    EventLoopTestAccess::Stale(S::loop(server), old_token);
    S::CloseNotice(server, fd, old_id);
    S::Drain(server);
    Expect(S::size(server) == 1 && ::fcntl(fd, F_GETFD) >= 0 &&
               C::events(current) == 0,
           "old close/token cannot destroy or reach new owner");
    fresh.SendReadiness();
    S::loop(server).PollOnce(250);
    Expect(C::events(current) == 1, "new real readiness reaches TcpConnection");
    std::cout << "reuse: fd=" << fd << " old_id=" << old_id
              << " new_id=" << current.identity()
              << " stale=" << S::loop(server).counters().stale
              << " old_close_misdelete=0 old_event_misdelivery=0 new_callback="
              << C::events(current) << '\n';
  }
  Expect(FdCount() == before, "reuse resources restored");
  std::cout << "lifecycle: callback_alive=" << in_callback_fd_alive
            << " self=" << self_calls << " survivor=" << survivor_calls
            << " same_batch_stale=" << stale << " fd=" << before << "->"
            << FdCount() << '\n';
}

void FailuresAndRecovery() {
  const auto before = FdCount();
  int construction_or_setup_fail{}, listener_add_fail{}, add_fail{}, mod_fail{},
      recovery{};
  {
    EventLoop loop;
    Pair pair;
    const int fd = pair.observed.fd();
    try {
      TcpConnection bad(loop, std::move(pair.observed), 0, 0);
      struct OnConnectionClosedObserver8 {
        void OnConnectionClosed(int, TcpConnection::Identity) {}
      };
      bad.set_OnConnectionClosed_callback(
          std::bind_front(&OnConnectionClosedObserver8::OnConnectionClosed,
                          OnConnectionClosedObserver8{}));
    } catch (const std::invalid_argument&) {
      ++construction_or_setup_fail;
    }
    Expect(::fcntl(fd, F_GETFD) == -1, "failed constructor reclaims socket");
    try {
      Acceptor invalid(loop, 0);
      invalid.Start();
    } catch (const std::invalid_argument&) {
      ++construction_or_setup_fail;
    }
    Acceptor broken_listener(loop, 0);
    struct AddConnectionObserver9 {
      void AddConnection(Socket) {}
    };
    broken_listener.set_AddConnection_callback(
        std::bind_front(&AddConnectionObserver9::AddConnection,
                        AddConnectionObserver9{}));
    AcceptorTestAccess::InvalidateListener(broken_listener);
    try {
      broken_listener.Start();
    } catch (const std::system_error&) {
      ++listener_add_fail;
    }
    Expect(
        listener_add_fail == 1,
        "listener registration failure propagates without fake registration");
    // /dev/null is a valid owned fd but cannot be epoll-registered (EPERM).
    Socket regular(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    int regular_fd = regular.fd();
    TcpConnection unsupported(loop, std::move(regular), 4, 0);
    struct OnConnectionClosedObserver10 {
      void OnConnectionClosed(int, TcpConnection::Identity) {}
    };
    unsupported.set_OnConnectionClosed_callback(
        std::bind_front(&OnConnectionClosedObserver10::OnConnectionClosed,
                        OnConnectionClosedObserver10{}));
    try {
      unsupported.Start();
    } catch (const std::system_error&) {
      ++add_fail;
    }
    Expect(unsupported.state() == TcpConnection::State::kUnregistered &&
               C::token(unsupported) == 0 && ::fcntl(regular_fd, F_GETFD) >= 0,
           "failed ADD does not fake active registration or close owner early");
  }
  {
    TcpServer server(0);
    Socket regular(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    int regular_fd = regular.fd();
    try {
      S::Add(server, std::move(regular));
    } catch (const std::system_error&) {
      ++add_fail;
    }
    Expect(S::size(server) == 0 && ::fcntl(regular_fd, F_GETFD) == -1,
           "server rolls back failed ADD object and fd");
    Pair broken;
    const int fd = broken.observed.fd();
    int size = 4096;
    Expect(::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0,
           "constrain failed MOD send buffer");
    S::Add(server, std::move(broken.observed));
    std::vector<std::byte> bytes(65536, std::byte{0x37});
    broken.Send(bytes);
    EventLoopTestAccess::DispatchAfterKernelDetach(S::loop(server), fd);
    if (S::size(server) == 0 && ::fcntl(fd, F_GETFD) == -1) ++mod_fail;
    Expect(mod_fail == 1,
           "real MOD failure closes only failed connection after dispatch");
    // This fixture pumps the loop directly; Run normally enables acceptance
    // after callback assembly and before entering that loop.
    S::StartAccepting(server);
    Socket client = ConnectTo(server.bound_port());
    S::loop(server).PollOnce(250);
    Expect(::send(client.fd(), "ok", 2, MSG_NOSIGNAL) == 2,
           "send after failures");
    S::loop(server).PollOnce(250);
    char output[2];
    pollfd readable{client.fd(), POLLIN, 0};
    Expect(::poll(&readable, 1, 1000) == 1, "await actual TCP echo delivery");
    if (::recv(client.fd(), output, 2, MSG_DONTWAIT) == 2 && output[0] == 'o' &&
        output[1] == 'k')
      ++recovery;
    Expect(recovery == 1,
           "real accepted connection succeeds after ADD/MOD failure");
  }
  Expect(FdCount() == before, "failure resources return to baseline");
  std::cout << "failures: constructors=" << construction_or_setup_fail
            << " listener_add=" << listener_add_fail << " add=" << add_fail
            << " mod=" << mod_fail << " recovery=" << recovery
            << " fd=" << before << "->" << FdCount() << '\n';
}

void RealReset() {
  EventLoop loop;
  Socket accepted;
  Acceptor acceptor(loop, 0);
  using AddConnectionObserver11State0 = decltype((accepted));
  struct AddConnectionObserver11 {
    AddConnectionObserver11State0 accepted;
    void AddConnection(Socket s) { accepted = std::move(s); }
  };
  acceptor.set_AddConnection_callback(
      std::bind_front(&AddConnectionObserver11::AddConnection,
                      AddConnectionObserver11{accepted}));
  acceptor.Start();
  Socket client = ConnectTo(acceptor.bound_port());
  loop.PollOnce(250);
  acceptor.Stop();
  Expect(accepted.valid(), "accept real reset TCP client");
  int notices{};
  TcpConnection c(loop, std::move(accepted), 7, 0);
  using OnConnectionClosedObserver12State0 = decltype((notices));
  struct OnConnectionClosedObserver12 {
    OnConnectionClosedObserver12State0 notices;
    void OnConnectionClosed(int, TcpConnection::Identity) { ++notices; }
  };
  c.set_OnConnectionClosed_callback(
      std::bind_front(&OnConnectionClosedObserver12::OnConnectionClosed,
                      OnConnectionClosedObserver12{notices}));
  c.Start();
  std::vector<std::byte> bytes(1053, std::byte{0x7a});
  Expect(::send(client.fd(), bytes.data(), bytes.size(), MSG_NOSIGNAL) == 1053,
         "queue real reset bytes");
  std::vector<std::byte> peeked(1053);
  ssize_t n = -1;
  for (int i = 0; i < 8; ++i) {
    n = ::recv(c.fd(), peeked.data(), peeked.size(), MSG_PEEK);
    if (n == 1053) break;
    ::usleep(1000);
  }
  Expect(n == 1053 && peeked == bytes, "queued bytes before TCP reset");
  // Poll for ERR on an independent observer without consuming SO_ERROR/data.
  // The target TcpConnection is still untouched until the real combined event.
  Epoller reset_ready;
  reset_ready.Add(c.fd(), 0, 1);
  linger reset{1, 0};
  Expect(
      ::setsockopt(client.fd(), SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) ==
          0,
      "reset linger");
  client.Reset();
  bool error_ready = false;
  for (int i = 0; i < 8 && !error_ready; ++i)
    for (const auto& e : reset_ready.Wait(250))
      if (e.events & EPOLLERR) error_ready = true;
  Expect(error_ready, "kernel TCP reset pending");
  reset_ready.Remove(c.fd());
  loop.PollOnce(250);
  const auto result = C::result(c);
  const bool combined =
      (C::mask(c) & (EPOLLERR | EPOLLIN)) == (EPOLLERR | EPOLLIN);
  Expect(
      combined && result.socket_error_observed &&
          result.socket_error == ECONNRESET &&
          result.bytes_read == bytes.size() && notices == 1,
      "TcpConnection full ERR/IN -> SO_ERROR -> exact recv -> deferred close");
  std::cout << "reset: tcp_connection_callbacks=" << C::events(c)
            << " combined=" << combined << " so_error=" << result.socket_error
            << " recv_bytes=" << result.bytes_read
            << " close_notice=" << notices << '\n';
}
}  // namespace

int main() {
  AcceptorDelivery();
  BuffersAndEcho();
  ApplicationBoundaries();
  CloseBatchAndReuse();
  FailuresAndRecovery();
  RealReset();
  std::cout << "Acceptor/TcpConnection assertions_failed=" << failures << '\n';
  return failures ? 1 : 0;
}
