#include "net/channel.h"
#include "net/event_loop.h"
#include "net/tcp_server.h"
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace hp::net {
// Controlled stale payload seam: uses the exact production token dispatch path.
struct EventLoopTestAccess {
    static void dispatch(EventLoop& loop, std::uint64_t token, std::uint32_t mask) {
        loop.dispatch(token, mask);
    }
};
}
namespace {
using namespace hp::net;
int failures{};
void expect(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
struct Pair {
    Socket observed, peer;
    Pair() {
        int fds[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds))
            throw std::runtime_error("socketpair");
        observed.reset(fds[0]); peer.reset(fds[1]);
    }
    void ready() { expect(::send(peer.fd(), "x", 1, MSG_NOSIGNAL) == 1, "send readiness"); }
};
void interest_and_remove() {
    EventLoop loop;
    Pair pair;
    int reads{}, writes{};
    Channel channel(loop, pair.observed.fd(), [&](std::uint32_t mask) {
        expect(mask == channel.revents(), "full revents retained");
        if (mask & EPOLLIN) {
            char c;
            expect(::recv(pair.observed.fd(), &c, 1, 0) == 1, "read byte");
            ++reads;
        }
        if (mask & EPOLLOUT) ++writes;
    });
    channel.set_interest(EPOLLIN);
    channel.set_interest(EPOLLIN);
    pair.ready(); loop.poll_once(250);
    channel.set_interest(EPOLLIN | EPOLLOUT); loop.poll_once(250);
    channel.set_interest(EPOLLIN);
    for (int i = 0; i < 10; ++i) loop.poll_once(0);
    expect(reads == 1 && writes == 1, "read/write and no writable busy-loop");
    channel.remove(); channel.remove();
    pair.ready();
    const auto before = loop.counters().dispatches;
    for (int i = 0; i < 10; ++i) loop.poll_once(0);
    expect(loop.counters().dispatches == before, "no callback after removal");
    const auto c = loop.counters();
    expect(c.adds == 1 && c.mods == 2 && c.removes == 1, "ADD/MOD/DEL and idempotence");
    std::cout << "interest: add=" << c.adds << " mod=" << c.mods << " del=" << c.removes
              << " read=" << reads << " write=" << writes << " disable_write=1 empty_polls=20 post_remove=0\n";
}
void callback_lifetime_and_same_batch() {
    EventLoop loop;
    Pair first, second;
    int self_callbacks{}, other_callbacks{}, deferred_destroy{};
    bool returned = false;
    std::unique_ptr<Channel> self;
    self = std::make_unique<Channel>(loop, first.observed.fd(), [&](std::uint32_t) {
        ++self_callbacks;
        self->remove();
        expect(!self->registered() && ::fcntl(first.observed.fd(), F_GETFD) >= 0,
               "invalidate registry before fd close");
        returned = true;
    });
    Channel other(loop, second.observed.fd(), [&](std::uint32_t) {
        ++other_callbacks; other.remove();
    });
    loop.set_after_dispatch([&] {
        if (returned && self) {
            self.reset();
            expect(::fcntl(first.observed.fd(), F_GETFD) >= 0, "Channel destruction does not close fd");
            first.observed.reset();
            ++deferred_destroy;
        }
    });
    self->set_interest(EPOLLIN); other.set_interest(EPOLLIN);
    first.ready(); second.ready();
    loop.poll_once(250);
    expect(self_callbacks == 1 && other_callbacks == 1 && deferred_destroy == 1,
           "current callback returns before destruction; same batch survivor dispatched");
    other.remove();
    std::cout << "lifetime: self_remove=" << self_callbacks << " same_batch_other="
              << other_callbacks << " deferred_destroy=" << deferred_destroy << "\n";

    // Whichever event arrives first removes both observers. The remaining real
    // kernel event in this batch must miss, independently of epoll event order.
    EventLoop batch;
    Pair a, b;
    int callbacks{};
    Channel* ca_ptr{}; Channel* cb_ptr{};
    auto callback = [&](std::uint32_t) { ++callbacks; ca_ptr->remove(); cb_ptr->remove(); };
    Channel ca(batch, a.observed.fd(), callback), cb(batch, b.observed.fd(), callback);
    ca_ptr = &ca; cb_ptr = &cb;
    ca.set_interest(EPOLLIN); cb.set_interest(EPOLLIN);
    a.ready(); b.ready(); batch.poll_once(250);
    ca.remove(); cb.remove();
    expect(callbacks == 1 && batch.counters().stale == 1, "same batch removed token skipped");
    std::cout << "batch: dispatch=" << callbacks << " stale=" << batch.counters().stale << "\n";
}
void fd_reuse_and_failures() {
    EventLoop loop;
    Pair old;
    const int reused_fd = old.observed.fd();
    int old_callbacks{}, new_callbacks{};
    std::uint64_t old_token{};
    {
        Channel channel(loop, reused_fd, [&](std::uint32_t) { ++old_callbacks; });
        channel.set_interest(EPOLLIN); old_token = channel.token(); channel.remove();
    }
    expect(::fcntl(reused_fd, F_GETFD) >= 0, "non-owning destructor");
    old.observed.reset();
    Pair fresh;
    if (fresh.observed.fd() != reused_fd) {
        expect(::dup2(fresh.observed.fd(), reused_fd) == reused_fd, "controlled numeric fd reuse");
        fresh.observed.reset(reused_fd);
    }
    Channel replacement(loop, reused_fd, [&](std::uint32_t) {
        ++new_callbacks; char c; expect(::recv(reused_fd, &c, 1, 0) == 1, "new owner byte");
    });
    replacement.set_interest(EPOLLIN);
    const auto new_token = replacement.token();
    EventLoopTestAccess::dispatch(loop, old_token, EPOLLIN | EPOLLOUT);
    expect(new_callbacks == 0, "old token not delivered to new fd owner");
    fresh.ready(); loop.poll_once(250);
    expect(new_token != old_token && old_callbacks == 0 && new_callbacks == 1 && loop.counters().stale == 1,
           "fresh token isolates fd reuse");
    replacement.remove();
    std::cout << "reuse: fd=" << reused_fd << " old_token=" << old_token << " new_token=" << new_token
              << " stale=" << loop.counters().stale << " old_misdelivery=" << old_callbacks
              << " new_callback=" << new_callbacks << '\n';

    int errors{};
    Channel good(loop, reused_fd, [](std::uint32_t) {});
    good.set_interest(EPOLLIN);
    Channel duplicate(loop, reused_fd, [](std::uint32_t) {});
    try { duplicate.set_interest(EPOLLIN); } catch (const std::logic_error&) { ++errors; }
    expect(!duplicate.registered() && duplicate.interest() == 0, "duplicate ADD fails without state commit");
    EventLoop wrong;
    try { wrong.update_channel(good, EPOLLOUT); } catch (const std::invalid_argument&) { ++errors; }
    // Remove the underlying fd to trigger a real kernel MOD failure. Channel is
    // deliberately non-owning; owner close here is fault injection, not usage.
    fresh.observed.reset();
    try { good.set_interest(EPOLLIN | EPOLLOUT); } catch (const std::system_error&) { ++errors; }
    expect(good.registered() && good.interest() == EPOLLIN, "failed MOD retains committed state");
    good.remove();
    Channel invalid(loop, reused_fd, [](std::uint32_t) {});
    try { invalid.set_interest(EPOLLIN); } catch (const std::system_error&) { ++errors; }
    expect(!invalid.registered() && invalid.interest() == 0, "kernel ADD failure rolls back registry");
    expect(errors == 4, "controlled registration/update/ownership failures");
    std::cout << "failures: controlled=" << errors << " false_registration=0\n";
}
int connect_loopback(std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in address{}; address.sin_family = AF_INET;
    address.sin_port = htons(port); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd < 0 || ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
        throw std::runtime_error("connect");
    return fd;
}
void real_reset_through_channel() {
    EventLoop loop;
    Socket listener = Socket::create_tcp();
    listener.bind_any(0); listener.listen(4);
    Socket accepted;
    int listener_callbacks{}, connection_callbacks{}, combined{}, so_errors{};
    Channel listening(loop, listener.fd(), [&](std::uint32_t mask) {
        expect(mask & EPOLLIN, "real listener read mask");
        ++listener_callbacks; accepted = listener.accept_non_blocking(); listening.remove();
    });
    listening.set_interest(EPOLLIN);
    Socket client(connect_loopback(listener.local_port()));
    loop.poll_once(250);
    listening.remove();
    expect(accepted.valid(), "Channel listener accepted TCP connection");
    ConnectionIo io(std::move(accepted));
    bool consume = false, error_ready = false;
    ConnectionEventResult result;
    Channel connection(loop, io.fd(), [&](std::uint32_t mask) {
        ++connection_callbacks;
        if (mask & EPOLLERR) error_ready = true;
        if (consume) {
            expect(mask == connection.revents(), "reset mask preserved in Channel");
            if ((mask & (EPOLLERR | EPOLLIN)) == (EPOLLERR | EPOLLIN)) ++combined;
            result = io.handle_event(mask);
            if (result.socket_error_observed && result.socket_error == ECONNRESET) ++so_errors;
            connection.remove();
        }
    });
    connection.set_interest(EPOLLIN | EPOLLRDHUP);
    std::vector<std::byte> payload(1053, std::byte{0x5a});
    expect(::send(client.fd(), payload.data(), payload.size(), MSG_NOSIGNAL) == 1053, "queue TCP bytes before reset");
    std::vector<std::byte> peeked(payload.size());
    ssize_t count{};
    for (int i=0; i<8; ++i) {
        count = ::recv(io.fd(), peeked.data(), peeked.size(), MSG_PEEK);
        if (count == 1053) break;
        loop.poll_once(250);
    }
    expect(count == 1053 && peeked == payload, "all TCP bytes queued without consuming");
    connection.set_interest(0);
    linger reset{1, 0};
    expect(::setsockopt(client.fd(), SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0, "zero linger");
    client.reset();
    for (int i=0; i<8 && !error_ready; ++i) loop.poll_once(250);
    expect(error_ready, "kernel reset pending");
    consume = true;
    connection.set_interest(EPOLLIN | EPOLLRDHUP);
    loop.poll_once(250);
    connection.remove();
    expect(combined == 1 && so_errors == 1 && result.bytes_read == payload.size() && result.close_requested,
           "Channel -> SO_ERROR ECONNRESET -> all queued bytes read before close");
    expect(io.pending_bytes() + result.bytes_written == payload.size(), "queued bytes preserved");
    std::cout << "reset: listener=" << listener_callbacks << " connection=" << connection_callbacks
              << " combined_err_in=" << combined << " so_error_econnreset=" << so_errors
              << " recv_bytes=" << result.bytes_read << " dispatch=" << loop.counters().dispatches << '\n';
}
void callback_exception() {
    EventLoop loop;
    Pair pair;
    int cleanup{}, caught{};
    Channel channel(loop, pair.observed.fd(), [&](std::uint32_t) {
        channel.remove(); throw std::runtime_error("callback failure");
    });
    loop.set_after_dispatch([&] { ++cleanup; });
    channel.set_interest(EPOLLIN); pair.ready();
    try { loop.poll_once(250); } catch (const std::runtime_error&) { ++caught; }
    channel.remove(); loop.poll_once(0);
    expect(cleanup == 1 && caught == 1, "exception cleanup and polling guard restored");
    std::cout << "exception: cleanup=" << cleanup << " propagated=" << caught << '\n';
}
}
int main() {
    interest_and_remove(); callback_lifetime_and_same_batch(); fd_reuse_and_failures();
    real_reset_through_channel(); callback_exception();
    std::cout << "EventLoop/Channel assertions_failed=" << failures << '\n';
    return failures ? 1 : 0;
}
