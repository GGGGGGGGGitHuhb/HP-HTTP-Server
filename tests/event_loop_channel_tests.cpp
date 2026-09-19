#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

#include "net/channel.h"
#include "net/event_loop.h"
#include "net/tcp_server.h"

namespace hp::net {
// Controlled stale payload seam: uses the exact production token dispatch path.
struct TcpConnectionTestAccess {
  static ConnectionEventResult event(TcpConnection& c, std::uint32_t mask) {
    c.HandleConnectionEvent(mask);
    return c.last_result_;
  }
};

struct EventLoopTestAccess {
  static void dispatch(EventLoop& loop,
                       std::uint64_t token,
                       std::uint32_t mask) {
    loop.Dispatch(token, mask);
  }
};
}  // namespace hp::net

namespace {
using namespace hp::net;
int failures{};

void expect(bool condition, const char* message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
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

  void ready() {
    expect(::send(peer.fd(), "x", 1, MSG_NOSIGNAL) == 1, "send readiness");
  }
};

void interest_and_remove() {
  EventLoop loop;
  Pair pair;
  int reads{}, writes{};
  Channel channel(loop, pair.observed.fd());
  using HandleConnectionEventObserver1State0 = decltype((channel));
  using HandleConnectionEventObserver1State1 = decltype((pair));
  using HandleConnectionEventObserver1State2 = decltype((reads));
  using HandleConnectionEventObserver1State3 = decltype((writes));
  struct HandleConnectionEventObserver1 {
    HandleConnectionEventObserver1State0 channel;
    HandleConnectionEventObserver1State1 pair;
    HandleConnectionEventObserver1State2 reads;
    HandleConnectionEventObserver1State3 writes;
    void HandleConnectionEvent(std::uint32_t mask) {
      expect(mask == channel.revents(), "full revents retained");
      if (mask & EPOLLIN) {
        char c;
        expect(::recv(pair.observed.fd(), &c, 1, 0) == 1, "read byte");
        ++reads;
      }
      if (mask & EPOLLOUT) ++writes;
    }
  };
  channel.set_HandleConnectionEvent_callback(std::bind_front(
      &HandleConnectionEventObserver1::HandleConnectionEvent,
      HandleConnectionEventObserver1{channel, pair, reads, writes}));
  channel.set_interest(EPOLLIN);
  channel.set_interest(EPOLLIN);
  pair.ready();
  loop.PollOnce(250);
  channel.set_interest(EPOLLIN | EPOLLOUT);
  loop.PollOnce(250);
  channel.set_interest(EPOLLIN);
  for (int i = 0; i < 10; ++i) loop.PollOnce(0);
  expect(reads == 1 && writes == 1, "read/write and no writable busy-loop");
  channel.Remove();
  channel.Remove();
  pair.ready();
  const auto before = loop.counters().dispatches;
  for (int i = 0; i < 10; ++i) loop.PollOnce(0);
  expect(loop.counters().dispatches == before, "no callback after removal");
  const auto c = loop.counters();
  expect(c.adds == 1 && c.mods == 2 && c.removes == 1,
         "ADD/MOD/DEL and idempotence");
  std::cout << "interest: add=" << c.adds << " mod=" << c.mods
            << " del=" << c.removes << " read=" << reads << " write=" << writes
            << " disable_write=1 empty_polls=20 post_remove=0\n";
}

void callback_lifetime_and_same_batch() {
  EventLoop loop;
  Pair first, second;
  int self_callbacks{}, other_callbacks{}, deferred_destroy{};
  bool returned = false;
  std::unique_ptr<Channel> self;
  self = std::make_unique<Channel>(loop, first.observed.fd());
  struct SelfRemovingChannel {
    int& self_callbacks;
    std::unique_ptr<Channel>& self;
    Pair& first;
    bool& returned;
    void HandleConnectionEvent(std::uint32_t) {
      ++self_callbacks;
      self->Remove();
      expect(!self->registered() && ::fcntl(first.observed.fd(), F_GETFD) >= 0,
             "invalidate registry before fd close");
      returned = true;
    }
  };
  self->set_HandleConnectionEvent_callback(std::bind_front(
      &SelfRemovingChannel::HandleConnectionEvent,
      SelfRemovingChannel{self_callbacks, self, first, returned}));
  Channel other(loop, second.observed.fd());
  using HandleConnectionEventObserver2State0 = decltype((other_callbacks));
  using HandleConnectionEventObserver2State1 = decltype((other));
  struct HandleConnectionEventObserver2 {
    HandleConnectionEventObserver2State0 other_callbacks;
    HandleConnectionEventObserver2State1 other;
    void HandleConnectionEvent(std::uint32_t) {
      ++other_callbacks;
      other.Remove();
    }
  };
  other.set_HandleConnectionEvent_callback(
      std::bind_front(&HandleConnectionEventObserver2::HandleConnectionEvent,
                      HandleConnectionEventObserver2{other_callbacks, other}));
  struct DeferredChannelCleanup {
    bool& returned;
    std::unique_ptr<Channel>& self;
    Pair& first;
    int& deferred_destroy;
    void DrainClosedConnections() {
      if (returned && self) {
        self.reset();
        expect(::fcntl(first.observed.fd(), F_GETFD) >= 0,
               "Channel destruction does not close fd");
        first.observed.Reset();
        ++deferred_destroy;
      }
    }
  } cleanup_target{returned, self, first, deferred_destroy};
  loop.set_DrainClosedConnections_callback(
      std::bind_front(&DeferredChannelCleanup::DrainClosedConnections,
                      &cleanup_target));
  self->set_interest(EPOLLIN);
  other.set_interest(EPOLLIN);
  first.ready();
  second.ready();
  loop.PollOnce(250);
  expect(self_callbacks == 1 && other_callbacks == 1 && deferred_destroy == 1,
         "current callback returns before destruction; same batch survivor "
         "dispatched");
  other.Remove();
  std::cout << "lifetime: self_remove=" << self_callbacks
            << " same_batch_other=" << other_callbacks
            << " deferred_destroy=" << deferred_destroy << "\n";

  // Whichever event arrives first removes both observers. The remaining real
  // kernel event in this batch must miss, independently of epoll event order.
  EventLoop batch;
  Pair a, b;
  int callbacks{};
  Channel* ca_ptr{};
  Channel* cb_ptr{};
  struct BatchRemoval {
    int& callbacks;
    Channel*& ca_ptr;
    Channel*& cb_ptr;
    void HandleConnectionEvent(std::uint32_t) {
      ++callbacks;
      ca_ptr->Remove();
      cb_ptr->Remove();
    }
  };
  auto callback = std::bind_front(&BatchRemoval::HandleConnectionEvent,
                                  BatchRemoval{callbacks, ca_ptr, cb_ptr});
  Channel ca(batch, a.observed.fd()), cb(batch, b.observed.fd());
  ca.set_HandleConnectionEvent_callback(callback);
  cb.set_HandleConnectionEvent_callback(callback);
  ca_ptr = &ca;
  cb_ptr = &cb;
  ca.set_interest(EPOLLIN);
  cb.set_interest(EPOLLIN);
  a.ready();
  b.ready();
  batch.PollOnce(250);
  ca.Remove();
  cb.Remove();
  expect(callbacks == 1 && batch.counters().stale == 1,
         "same batch removed token skipped");
  std::cout << "batch: dispatch=" << callbacks
            << " stale=" << batch.counters().stale << "\n";
}

void fd_reuse_and_failures() {
  EventLoop loop;
  Pair old;
  const int reused_fd = old.observed.fd();
  int old_callbacks{}, new_callbacks{};
  std::uint64_t old_token{};
  {
    Channel channel(loop, reused_fd);
    using HandleConnectionEventObserver3State0 = decltype((old_callbacks));
    struct HandleConnectionEventObserver3 {
      HandleConnectionEventObserver3State0 old_callbacks;
      void HandleConnectionEvent(std::uint32_t) { ++old_callbacks; }
    };
    channel.set_HandleConnectionEvent_callback(
        std::bind_front(&HandleConnectionEventObserver3::HandleConnectionEvent,
                        HandleConnectionEventObserver3{old_callbacks}));
    channel.set_interest(EPOLLIN);
    old_token = channel.token();
    channel.Remove();
  }
  expect(::fcntl(reused_fd, F_GETFD) >= 0, "non-owning destructor");
  old.observed.Reset();
  Pair fresh;
  if (fresh.observed.fd() != reused_fd) {
    expect(::dup2(fresh.observed.fd(), reused_fd) == reused_fd,
           "controlled numeric fd reuse");
    fresh.observed.Reset(reused_fd);
  }
  Channel replacement(loop, reused_fd);
  using HandleConnectionEventObserver4State0 = decltype((new_callbacks));
  using HandleConnectionEventObserver4State1 = decltype((reused_fd));
  struct HandleConnectionEventObserver4 {
    HandleConnectionEventObserver4State0 new_callbacks;
    HandleConnectionEventObserver4State1 reused_fd;
    void HandleConnectionEvent(std::uint32_t) {
      ++new_callbacks;
      char c;
      expect(::recv(reused_fd, &c, 1, 0) == 1, "new owner byte");
    }
  };
  replacement.set_HandleConnectionEvent_callback(std::bind_front(
      &HandleConnectionEventObserver4::HandleConnectionEvent,
      HandleConnectionEventObserver4{new_callbacks, reused_fd}));
  replacement.set_interest(EPOLLIN);
  const auto new_token = replacement.token();
  EventLoopTestAccess::dispatch(loop, old_token, EPOLLIN | EPOLLOUT);
  expect(new_callbacks == 0, "old token not delivered to new fd owner");
  fresh.ready();
  loop.PollOnce(250);
  expect(new_token != old_token && old_callbacks == 0 && new_callbacks == 1 &&
             loop.counters().stale == 1,
         "fresh token isolates fd reuse");
  replacement.Remove();
  std::cout << "reuse: fd=" << reused_fd << " old_token=" << old_token
            << " new_token=" << new_token << " stale=" << loop.counters().stale
            << " old_misdelivery=" << old_callbacks
            << " new_callback=" << new_callbacks << '\n';

  int errors{};
  Channel good(loop, reused_fd);
  struct HandleConnectionEventObserver5 {
    void HandleConnectionEvent(std::uint32_t) {}
  };
  good.set_HandleConnectionEvent_callback(
      std::bind_front(&HandleConnectionEventObserver5::HandleConnectionEvent,
                      HandleConnectionEventObserver5{}));
  good.set_interest(EPOLLIN);
  Channel duplicate(loop, reused_fd);
  struct HandleConnectionEventObserver6 {
    void HandleConnectionEvent(std::uint32_t) {}
  };
  duplicate.set_HandleConnectionEvent_callback(
      std::bind_front(&HandleConnectionEventObserver6::HandleConnectionEvent,
                      HandleConnectionEventObserver6{}));
  try {
    duplicate.set_interest(EPOLLIN);
  } catch (const std::logic_error&) {
    ++errors;
  }
  expect(!duplicate.registered() && duplicate.interest() == 0,
         "duplicate ADD fails without state commit");
  EventLoop wrong;
  try {
    wrong.UpdateChannel(good, EPOLLOUT);
  } catch (const std::invalid_argument&) {
    ++errors;
  }
  // Remove the underlying fd to trigger a real kernel MOD failure. Channel is
  // deliberately non-owning; owner close here is fault injection, not usage.
  fresh.observed.Reset();
  try {
    good.set_interest(EPOLLIN | EPOLLOUT);
  } catch (const std::system_error&) {
    ++errors;
  }
  expect(good.registered() && good.interest() == EPOLLIN,
         "failed MOD retains committed state");
  good.Remove();
  Channel invalid(loop, reused_fd);
  struct HandleConnectionEventObserver7 {
    void HandleConnectionEvent(std::uint32_t) {}
  };
  invalid.set_HandleConnectionEvent_callback(
      std::bind_front(&HandleConnectionEventObserver7::HandleConnectionEvent,
                      HandleConnectionEventObserver7{}));
  try {
    invalid.set_interest(EPOLLIN);
  } catch (const std::system_error&) {
    ++errors;
  }
  expect(!invalid.registered() && invalid.interest() == 0,
         "kernel ADD failure rolls back registry");
  expect(errors == 4, "controlled registration/update/ownership failures");
  std::cout << "failures: controlled=" << errors << " false_registration=0\n";
}

int connect_loopback(std::uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (fd < 0 ||
      ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
    throw std::runtime_error("connect");
  return fd;
}

void real_reset_through_channel() {
  EventLoop loop;
  Socket listener = Socket::CreateTcp();
  listener.BindAny(0);
  listener.Listen(4);
  Socket accepted;
  int listener_callbacks{}, connection_callbacks{}, combined{}, so_errors{};
  Channel listening(loop, listener.fd());
  using HandleConnectionEventObserver8State0 = decltype((listener_callbacks));
  using HandleConnectionEventObserver8State1 = decltype((accepted));
  using HandleConnectionEventObserver8State2 = decltype((listener));
  using HandleConnectionEventObserver8State3 = decltype((listening));
  struct HandleConnectionEventObserver8 {
    HandleConnectionEventObserver8State0 listener_callbacks;
    HandleConnectionEventObserver8State1 accepted;
    HandleConnectionEventObserver8State2 listener;
    HandleConnectionEventObserver8State3 listening;
    void HandleConnectionEvent(std::uint32_t mask) {
      expect(mask & EPOLLIN, "real listener read mask");
      ++listener_callbacks;
      accepted = listener.AcceptNonBlocking();
      listening.Remove();
    }
  };
  listening.set_HandleConnectionEvent_callback(
      std::bind_front(&HandleConnectionEventObserver8::HandleConnectionEvent,
                      HandleConnectionEventObserver8{listener_callbacks,
                                                     accepted,
                                                     listener,
                                                     listening}));
  listening.set_interest(EPOLLIN);
  Socket client(connect_loopback(listener.local_port()));
  loop.PollOnce(250);
  listening.Remove();
  expect(accepted.valid(), "Channel listener accepted TCP connection");
  EventLoop io_loop;
  TcpConnection io(io_loop, std::move(accepted), 1, 0);
  struct OnConnectionClosedObserver9 {
    void OnConnectionClosed(int, TcpConnection::Identity) {}
  };
  io.set_OnConnectionClosed_callback(
      std::bind_front(&OnConnectionClosedObserver9::OnConnectionClosed,
                      OnConnectionClosedObserver9{}));
  io.Start();
  bool consume = false, error_ready = false;
  ConnectionEventResult result;
  Channel connection(loop, io.fd());
  using HandleConnectionEventObserver10State0 =
      decltype((connection_callbacks));
  using HandleConnectionEventObserver10State1 = decltype((error_ready));
  using HandleConnectionEventObserver10State2 = decltype((consume));
  using HandleConnectionEventObserver10State3 = decltype((connection));
  using HandleConnectionEventObserver10State4 = decltype((combined));
  using HandleConnectionEventObserver10State5 = decltype((result));
  using HandleConnectionEventObserver10State6 = decltype((io));
  using HandleConnectionEventObserver10State7 = decltype((so_errors));
  struct HandleConnectionEventObserver10 {
    HandleConnectionEventObserver10State0 connection_callbacks;
    HandleConnectionEventObserver10State1 error_ready;
    HandleConnectionEventObserver10State2 consume;
    HandleConnectionEventObserver10State3 connection;
    HandleConnectionEventObserver10State4 combined;
    HandleConnectionEventObserver10State5 result;
    HandleConnectionEventObserver10State6 io;
    HandleConnectionEventObserver10State7 so_errors;
    void HandleConnectionEvent(std::uint32_t mask) {
      ++connection_callbacks;
      if (mask & EPOLLERR) error_ready = true;
      if (consume) {
        expect(mask == connection.revents(), "reset mask preserved in Channel");
        if ((mask & (EPOLLERR | EPOLLIN)) == (EPOLLERR | EPOLLIN)) ++combined;
        result = TcpConnectionTestAccess::event(io, mask);
        if (result.socket_error_observed && result.socket_error == ECONNRESET)
          ++so_errors;
        connection.Remove();
      }
    }
  };
  connection.set_HandleConnectionEvent_callback(
      std::bind_front(&HandleConnectionEventObserver10::HandleConnectionEvent,
                      HandleConnectionEventObserver10{connection_callbacks,
                                                      error_ready,
                                                      consume,
                                                      connection,
                                                      combined,
                                                      result,
                                                      io,
                                                      so_errors}));
  connection.set_interest(EPOLLIN | EPOLLRDHUP);
  std::vector<std::byte> payload(1053, std::byte{0x5a});
  expect(
      ::send(client.fd(), payload.data(), payload.size(), MSG_NOSIGNAL) == 1053,
      "queue TCP bytes before reset");
  std::vector<std::byte> peeked(payload.size());
  ssize_t count{};
  for (int i = 0; i < 8; ++i) {
    count = ::recv(io.fd(), peeked.data(), peeked.size(), MSG_PEEK);
    if (count == 1053) break;
    loop.PollOnce(250);
  }
  expect(count == 1053 && peeked == payload,
         "all TCP bytes queued without consuming");
  connection.set_interest(0);
  linger reset{1, 0};
  expect(
      ::setsockopt(client.fd(), SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) ==
          0,
      "zero linger");
  client.Reset();
  for (int i = 0; i < 8 && !error_ready; ++i) loop.PollOnce(250);
  expect(error_ready, "kernel reset pending");
  consume = true;
  connection.set_interest(EPOLLIN | EPOLLRDHUP);
  loop.PollOnce(250);
  connection.Remove();
  expect(
      combined == 1 && so_errors == 1 && result.bytes_read == payload.size() &&
          result.close_requested,
      "Channel -> SO_ERROR ECONNRESET -> all queued bytes read before close");
  expect(io.pending_bytes() + result.bytes_written == payload.size(),
         "queued bytes preserved");
  std::cout << "reset: listener=" << listener_callbacks
            << " connection=" << connection_callbacks
            << " combined_err_in=" << combined
            << " so_error_econnreset=" << so_errors
            << " recv_bytes=" << result.bytes_read
            << " dispatch=" << loop.counters().dispatches << '\n';
}

void callback_exception() {
  EventLoop loop;
  Pair pair;
  int cleanup{}, caught{};
  Channel channel(loop, pair.observed.fd());
  using HandleConnectionEventObserver11State0 = decltype((channel));
  struct HandleConnectionEventObserver11 {
    HandleConnectionEventObserver11State0 channel;
    void HandleConnectionEvent(std::uint32_t) {
      channel.Remove();
      throw std::runtime_error("callback failure");
    }
  };
  channel.set_HandleConnectionEvent_callback(
      std::bind_front(&HandleConnectionEventObserver11::HandleConnectionEvent,
                      HandleConnectionEventObserver11{channel}));
  struct CleanupCounter {
    int& count;
    void DrainClosedConnections() { ++count; }
  } cleanup_target{cleanup};
  loop.set_DrainClosedConnections_callback(
      std::bind_front(&CleanupCounter::DrainClosedConnections,
                      &cleanup_target));
  channel.set_interest(EPOLLIN);
  pair.ready();
  try {
    loop.PollOnce(250);
  } catch (const std::runtime_error&) {
    ++caught;
  }
  channel.Remove();
  loop.PollOnce(0);
  expect(cleanup == 1 && caught == 1,
         "exception cleanup and polling guard restored");
  std::cout << "exception: cleanup=" << cleanup << " propagated=" << caught
            << '\n';
}
void named_control_slot() {
  EventLoop loop;
  struct ControlTarget {
    EventLoop& loop;
    int main_calls{0}, worker_calls{0}, refused{0};
    void HandleControl(EventLoop::Control, EventLoop::Deadline) {
      ++main_calls;
    }
    void HandleWorkerControl(EventLoop::Control, EventLoop::Deadline) {
      ++worker_calls;
      try {
        loop.set_HandleControl_callback({});
      } catch (const std::logic_error&) {
        ++refused;
      }
      try {
        loop.set_HandleWorkerControl_callback({});
      } catch (const std::logic_error&) {
        ++refused;
      }
    }
  } target{loop};
  loop.set_HandleControl_callback(
      std::bind_front(&ControlTarget::HandleControl, &target));
  loop.set_HandleWorkerControl_callback(
      std::bind_front(&ControlTarget::HandleWorkerControl, &target));
  loop.RequestForce();
  loop.PollOnce(0);
  expect(
      target.main_calls == 0 && target.worker_calls == 1 && target.refused == 2,
      "named control setters share one slot and dispatch guard");
}

}  // namespace

int main() {
  named_control_slot();
  interest_and_remove();
  callback_lifetime_and_same_batch();
  fd_reuse_and_failures();
  real_reset_through_channel();
  callback_exception();
  std::cout << "EventLoop/Channel assertions_failed=" << failures << '\n';
  return failures ? 1 : 0;
}
