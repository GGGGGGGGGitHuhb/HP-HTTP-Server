#include "net/tcp_server.h"

#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::vector<std::byte> make_payload(std::size_t size) {
    std::vector<std::byte> payload(size);
    for (std::size_t index = 0; index < size; ++index) {
        payload[index] = static_cast<std::byte>((index * 131U + 17U) & 0xffU);
    }
    return payload;
}

void test_binary_read_echo_and_half_close() {
    int fds[2] = {-1, -1};
    expect(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                        fds) == 0,
           "binary socketpair must be created");
    if (fds[0] == -1) {
        return;
    }

    hp::net::ConnectionIo connection{hp::net::Socket(fds[0])};
    const auto payload = make_payload(48 * 1024 + 37);
    std::size_t sent = 0;
    while (sent < payload.size()) {
        const ssize_t count =
            ::send(fds[1], payload.data() + sent, payload.size() - sent, 0);
        if (count > 0) {
            sent += static_cast<std::size_t>(count);
            continue;
        }
        if (count == -1 && errno == EINTR) {
            continue;
        }
        expect(false, "binary payload must fit controlled socketpair");
        break;
    }
    expect(::shutdown(fds[1], SHUT_WR) == 0,
           "peer write half must shut down");

    const hp::net::ReadResult read = connection.read_available();
    expect(read.bytes_read == payload.size(),
           "read loop must consume all binary bytes across chunks");
    expect(read.peer_closed && connection.peer_half_closed(),
           "read EOF must mark peer half closed");
    expect(connection.pending_bytes() == payload.size(),
           "every read byte must enter output state");

    const hp::net::WriteResult written = connection.write_available();
    expect(written.bytes_written == payload.size(),
           "echo write must emit the full binary payload");
    expect(!connection.has_pending_output(),
           "successful echo must clear output state");
    expect(connection.ready_to_close(),
           "half-closed peer must become closable after drain");

    std::vector<std::byte> received(payload.size());
    std::size_t received_count = 0;
    while (received_count < received.size()) {
        const ssize_t count = ::recv(fds[1], received.data() + received_count,
                                     received.size() - received_count, 0);
        if (count > 0) {
            received_count += static_cast<std::size_t>(count);
            continue;
        }
        if (count == -1 && errno == EINTR) {
            continue;
        }
        expect(false, "echo payload must be readable in full");
        break;
    }
    expect(received == payload,
           "binary echo must preserve byte order, length and embedded zeros");
    ::close(fds[1]);
}

void test_short_write_and_eagain_resume() {
    int fds[2] = {-1, -1};
    expect(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                        fds) == 0,
           "short-write socketpair must be created");
    if (fds[0] == -1) {
        return;
    }

    int send_buffer = 4096;
    expect(::setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &send_buffer,
                        sizeof(send_buffer)) == 0,
           "controlled send buffer must be configured");

    hp::net::ConnectionIo connection{hp::net::Socket(fds[0])};
    const auto payload = make_payload(4 * 1024 * 1024 + 19);
    connection.queue_output(payload);
    const hp::net::WriteResult first = connection.write_available();
    expect(first.would_block, "paused peer must trigger write EAGAIN");
    expect(first.bytes_written > 0 && first.bytes_written < payload.size(),
           "first write must make partial positive progress before EAGAIN");
    expect(connection.pending_bytes() == payload.size() - first.bytes_written,
           "pending cursor must advance by exactly the bytes written");

    std::vector<std::byte> received;
    received.reserve(payload.size());
    std::vector<std::byte> chunk(64 * 1024);
    for (int attempt = 0;
         attempt < 20000 &&
         (connection.has_pending_output() || received.size() < payload.size());
         ++attempt) {
        while (true) {
            const ssize_t count = ::recv(fds[1], chunk.data(), chunk.size(), 0);
            if (count > 0) {
                received.insert(received.end(), chunk.begin(),
                                chunk.begin() + count);
                continue;
            }
            if (count == -1 && errno == EINTR) {
                continue;
            }
            break;
        }
        if (connection.has_pending_output()) {
            (void)connection.write_available();
        }
    }

    expect(!connection.has_pending_output(),
           "resumed writes must eventually drain output");
    expect(received.size() == payload.size(),
           "resumed peer must receive the original byte count");
    expect(received == payload,
           "short-write resume must not lose, duplicate or reorder bytes");
    ::close(fds[1]);
}

volatile std::sig_atomic_t signal_count = 0;

extern "C" void count_signal(int) {
    signal_count = 1;
}

void install_signal_handler(struct sigaction& old_action) {
    struct sigaction action {};
    action.sa_handler = count_signal;
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    expect(::sigaction(SIGUSR1, &action, &old_action) == 0,
           "SIGUSR1 handler must install");
}

void test_read_retries_eintr() {
    int fds[2] = {-1, -1};
    expect(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0,
           "blocking read socketpair must be created");
    if (fds[0] == -1) {
        return;
    }

    struct sigaction old_action {};
    install_signal_handler(old_action);
    const pid_t child = ::fork();
    expect(child >= 0, "read EINTR child must fork");
    if (child == 0) {
        ::close(fds[0]);
        ::usleep(20'000);
        ::kill(::getppid(), SIGUSR1);
        ::usleep(20'000);
        const char payload[] = "eintr-read";
        (void)::send(fds[1], payload, sizeof(payload), 0);
        (void)::shutdown(fds[1], SHUT_WR);
        ::close(fds[1]);
        _exit(0);
    }
    if (child < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return;
    }

    ::close(fds[1]);
    hp::net::ConnectionIo connection{hp::net::Socket(fds[0])};
    const hp::net::ReadResult read = connection.read_available();
    expect(signal_count > 0, "controlled signal must interrupt blocking recv");
    expect(read.bytes_read == sizeof("eintr-read") && read.peer_closed,
           "recv must retry EINTR, retain bytes and reach EOF");
    int status = 0;
    (void)::waitpid(child, &status, 0);
    expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "read EINTR helper must exit cleanly");
    (void)::sigaction(SIGUSR1, &old_action, nullptr);
}

void test_write_retries_eintr() {
    int fds[2] = {-1, -1};
    expect(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0,
           "blocking write socketpair must be created");
    if (fds[0] == -1) {
        return;
    }

    int send_buffer = 4096;
    expect(::setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &send_buffer,
                        sizeof(send_buffer)) == 0,
           "EINTR write send buffer must be constrained");
    const int original_flags = ::fcntl(fds[0], F_GETFL);
    expect(original_flags != -1 &&
               ::fcntl(fds[0], F_SETFL, original_flags | O_NONBLOCK) == 0,
           "sender must become non-blocking for deterministic prefill");

    std::vector<std::byte> filler(16 * 1024, std::byte{0x5a});
    std::size_t prefilled = 0;
    while (true) {
        const ssize_t count = ::send(fds[0], filler.data(), filler.size(),
                                     MSG_NOSIGNAL);
        if (count > 0) {
            prefilled += static_cast<std::size_t>(count);
            continue;
        }
        if (count == -1 && errno == EINTR) {
            continue;
        }
        expect(count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK),
               "socket prefill must stop at EAGAIN");
        break;
    }
    expect(prefilled > 0, "socket prefill must occupy the send buffer");
    expect(::fcntl(fds[0], F_SETFL, original_flags & ~O_NONBLOCK) == 0,
           "sender must return to blocking mode");

    struct sigaction old_action {};
    install_signal_handler(old_action);
    signal_count = 0;
    const auto payload = make_payload(512 * 1024 + 23);
    const pid_t child = ::fork();
    expect(child >= 0, "write EINTR helper must fork");
    if (child == 0) {
        ::close(fds[0]);
        ::usleep(20'000);
        ::kill(::getppid(), SIGUSR1);
        ::usleep(20'000);
        std::vector<std::byte> buffer(64 * 1024);
        std::size_t total = 0;
        const std::size_t expected = prefilled + payload.size();
        while (total < expected) {
            const ssize_t count = ::recv(fds[1], buffer.data(), buffer.size(), 0);
            if (count > 0) {
                total += static_cast<std::size_t>(count);
                continue;
            }
            if (count == -1 && errno == EINTR) {
                continue;
            }
            break;
        }
        ::close(fds[1]);
        _exit(total == expected ? 0 : 1);
    }
    if (child < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return;
    }

    ::close(fds[1]);
    hp::net::ConnectionIo connection{hp::net::Socket(fds[0])};
    connection.queue_output(payload);
    const hp::net::WriteResult write = connection.write_available();
    expect(signal_count > 0, "controlled signal must interrupt blocking send");
    expect(write.bytes_written == payload.size() &&
               !connection.has_pending_output(),
           "send must retry EINTR and drain the exact queued payload");
    int status = 0;
    (void)::waitpid(child, &status, 0);
    expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "write EINTR helper must receive prefill plus payload");
    (void)::sigaction(SIGUSR1, &old_action, nullptr);
}

void test_sigpipe_is_suppressed() {
    int fds[2] = {-1, -1};
    expect(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0,
           "SIGPIPE socketpair must be created");
    if (fds[0] == -1) {
        return;
    }
    hp::net::ConnectionIo connection{hp::net::Socket(fds[0])};
    ::close(fds[1]);
    const auto payload = make_payload(64);
    connection.queue_output(payload);
    const hp::net::WriteResult write = connection.write_available();
    expect(write.error_number == EPIPE || write.error_number == ECONNRESET,
           "closed peer write must preserve a connection error");
    expect(true, "MSG_NOSIGNAL must keep the test process alive");
}

int connect_loopback(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd == -1) {
        return -1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                  sizeof(address)) == -1) {
        ::close(fd);
        return -1;
    }
    return fd;
}

void test_real_epoll_error_preserves_same_batch_bytes() {
    hp::net::Socket listener = hp::net::Socket::create_tcp();
    listener.set_reuse_address(true);
    listener.bind_any(0);
    listener.listen(4);
    hp::net::Epoller epoller;
    constexpr std::uint64_t listener_token = 0x55aa7700ULL;
    constexpr std::uint64_t connection_token = 0x55aa7711ULL;
    epoller.add(listener.fd(), EPOLLIN, listener_token);

    const int client = connect_loopback(listener.local_port());
    expect(client >= 0, "RST combination client must connect");
    if (client < 0) {
        return;
    }
    bool listener_ready = false;
    for (int attempt = 0; attempt < 8 && !listener_ready; ++attempt) {
        for (const epoll_event& event : epoller.wait(250)) {
            if (event.data.u64 == listener_token &&
                (event.events & EPOLLIN) != 0U) {
                listener_ready = true;
            }
        }
    }
    expect(listener_ready, "RST combination listener must become readable");
    hp::net::Socket accepted = listener.accept_non_blocking();
    expect(accepted.valid(), "RST combination connection must be accepted");
    if (!accepted.valid()) {
        ::close(client);
        return;
    }

    epoller.remove(listener.fd());
    epoller.add(accepted.fd(), EPOLLIN | EPOLLRDHUP, connection_token);

    const auto payload = make_payload(1024 + 29);
    std::size_t sent = 0;
    while (sent < payload.size()) {
        const ssize_t count =
            ::send(client, payload.data() + sent, payload.size() - sent,
                   MSG_NOSIGNAL);
        if (count > 0) {
            sent += static_cast<std::size_t>(count);
            continue;
        }
        if (count == -1 && errno == EINTR) {
            continue;
        }
        expect(false, "RST combination payload must be sent before reset");
        break;
    }

    std::vector<std::byte> peeked(payload.size());
    ssize_t peeked_bytes = -1;
    for (int attempt = 0; attempt < 8; ++attempt) {
        peeked_bytes =
            ::recv(accepted.fd(), peeked.data(), peeked.size(), MSG_PEEK);
        if (peeked_bytes == static_cast<ssize_t>(payload.size())) {
            break;
        }
        if (peeked_bytes == -1 && errno != EINTR && errno != EAGAIN &&
            errno != EWOULDBLOCK) {
            break;
        }
        (void)epoller.wait(250);
    }
    expect(peeked_bytes == static_cast<ssize_t>(payload.size()),
           "RST combination payload must be fully queued before reset");
    if (peeked_bytes == static_cast<ssize_t>(payload.size())) {
        expect(std::equal(peeked.begin(), peeked.end(), payload.begin()),
               "MSG_PEEK must observe the exact queued payload without consuming it");
    }

    // ERR/HUP are reported regardless of the requested interest. Suppress the
    // level-triggered readable wakeup while the real TCP reset becomes pending.
    epoller.modify(accepted.fd(), 0U, connection_token);
    linger reset_linger{1, 0};
    expect(::setsockopt(client, SOL_SOCKET, SO_LINGER, &reset_linger,
                        sizeof(reset_linger)) == 0,
           "RST combination must configure zero linger");
    ::close(client);

    bool error_ready = false;
    for (int attempt = 0; attempt < 8 && !error_ready; ++attempt) {
        for (const epoll_event& event : epoller.wait(250)) {
            if (event.data.u64 == connection_token &&
                (event.events & EPOLLERR) != 0U) {
                error_ready = true;
            }
        }
    }
    expect(error_ready, "real reset must make EPOLLERR observable");
    // Restoring the production interest obtains a kernel-produced same-batch
    // EPOLLIN|EPOLLERR mask while the peeked bytes remain unread.
    epoller.modify(accepted.fd(), EPOLLIN | EPOLLRDHUP, connection_token);

    std::uint32_t observed_events = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const auto events = epoller.wait(250);
        for (const epoll_event& event : events) {
            if (event.data.u64 == connection_token &&
                (event.events & EPOLLIN) != 0U &&
                (event.events & EPOLLERR) != 0U) {
                observed_events = event.events;
            }
        }
        if (observed_events != 0U) {
            break;
        }
    }

    expect((observed_events & EPOLLIN) != 0U,
           "real reset event must contain EPOLLIN for queued bytes");
    expect((observed_events & EPOLLERR) != 0U,
           "real reset event must contain EPOLLERR");

    hp::net::ConnectionIo connection{std::move(accepted)};
    const hp::net::ConnectionEventResult result =
        connection.handle_event(observed_events);
    expect(result.socket_error_observed,
           "production event path must query SO_ERROR for EPOLLERR");
    expect(result.socket_error == ECONNRESET,
           "SO_ERROR must preserve the real ECONNRESET diagnosis");
    expect(result.bytes_read == payload.size(),
           "production event path must consume every same-batch byte before close");
    expect(result.close_requested,
           "ERR/HUP production event path must request connection close");
    expect(connection.pending_bytes() + result.bytes_written == payload.size(),
           "same-batch bytes must be retained or written, never discarded");
    epoller.remove(connection.fd());
}
}  // namespace

int main() {
    test_binary_read_echo_and_half_close();
    test_short_write_and_eagain_resume();
    test_read_retries_eintr();
    test_write_retries_eintr();
    test_sigpipe_is_suppressed();
    test_real_epoll_error_preserves_same_batch_bytes();

    if (failures != 0) {
        std::cerr << failures << " connection IO assertion(s) failed\n";
        return 1;
    }
    std::cout << "Connection IO tests passed\n";
    return 0;
}
