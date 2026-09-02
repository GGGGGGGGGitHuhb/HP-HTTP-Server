#include "net/epoller.h"
#include "net/socket.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <system_error>
#include <poll.h>
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

bool is_non_blocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL);
    return flags != -1 && (flags & O_NONBLOCK) != 0;
}

bool is_close_on_exec(int fd) {
    const int flags = ::fcntl(fd, F_GETFD);
    return flags != -1 && (flags & FD_CLOEXEC) != 0;
}

void test_epoller_lifecycle_and_lt() {
    hp::net::Epoller epoller(4);
    expect(is_close_on_exec(epoller.fd()),
           "epoll fd must have close-on-exec semantics");

    int pipe_fds[2] = {-1, -1};
    expect(::pipe2(pipe_fds, O_NONBLOCK | O_CLOEXEC) == 0,
           "pipe2 must succeed for LT test");
    if (pipe_fds[0] == -1) {
        return;
    }

    constexpr std::uint64_t token = 0x12345678ULL;
    epoller.add(pipe_fds[0], EPOLLIN, token);
    const char byte = 'x';
    expect(::write(pipe_fds[1], &byte, 1) == 1,
           "LT test byte must be written");

    const auto first = epoller.wait(0);
    expect(first.size() == 1, "first LT wait must observe unread data");
    if (!first.empty()) {
        expect(first.front().data.u64 == token,
               "epoll must preserve caller token");
        expect((first.front().events & EPOLLIN) != 0U,
               "first LT event must be readable");
    }

    const auto second = epoller.wait(0);
    expect(second.size() == 1,
           "LT must report the same unread level on a second wait");

    epoller.modify(pipe_fds[0], EPOLLIN, token + 1);
    const auto modified = epoller.wait(0);
    expect(modified.size() == 1 && modified.front().data.u64 == token + 1,
           "modify must replace the observation token");

    char received = 0;
    expect(::read(pipe_fds[0], &received, 1) == 1,
           "LT test byte must be drained");
    expect(epoller.wait(0).empty(), "drained level must no longer be ready");

    epoller.remove(pipe_fds[0]);
    epoller.remove(pipe_fds[0]);
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
}

volatile std::sig_atomic_t signal_count = 0;

extern "C" void count_signal(int) {
    signal_count = 1;
}

void test_epoll_wait_retries_eintr() {
    struct sigaction action {};
    action.sa_handler = count_signal;
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    struct sigaction old_action {};
    expect(::sigaction(SIGALRM, &action, &old_action) == 0,
           "SIGALRM handler must install");

    hp::net::Epoller epoller;
    ::ualarm(10'000, 0);
    const auto events = epoller.wait(30);
    ::ualarm(0, 0);

    expect(events.empty(), "EINTR retry wait must eventually time out empty");
    expect(signal_count > 0, "controlled signal must interrupt epoll_wait");
    expect(::sigaction(SIGALRM, &old_action, nullptr) == 0,
           "SIGALRM handler must restore");
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

void test_accept_retries_eintr() {
    const int raw_listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    expect(raw_listener >= 0, "blocking listener socket must be created");
    if (raw_listener < 0) {
        return;
    }
    hp::net::Socket listener(raw_listener);
    listener.set_reuse_address(true);
    listener.bind_any(0);
    listener.listen(4);

    struct sigaction action {};
    action.sa_handler = count_signal;
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    struct sigaction old_action {};
    expect(::sigaction(SIGUSR2, &action, &old_action) == 0,
           "SIGUSR2 handler must install");
    signal_count = 0;

    const pid_t child = ::fork();
    expect(child >= 0, "accept EINTR helper must fork");
    if (child == 0) {
        ::usleep(20'000);
        ::kill(::getppid(), SIGUSR2);
        ::usleep(20'000);
        const int client = connect_loopback(listener.local_port());
        if (client >= 0) {
            ::close(client);
        }
        _exit(client >= 0 ? 0 : 1);
    }
    if (child < 0) {
        (void)::sigaction(SIGUSR2, &old_action, nullptr);
        return;
    }

    hp::net::Socket accepted = listener.accept_non_blocking();
    expect(signal_count > 0,
           "controlled signal must interrupt blocking accept4");
    expect(accepted.valid(), "accept4 must retry EINTR and accept the client");
    if (accepted.valid()) {
        expect(is_non_blocking(accepted.fd()),
               "EINTR-retried accepted fd must be non-blocking");
        expect(is_close_on_exec(accepted.fd()),
               "EINTR-retried accepted fd must be close-on-exec");
    }
    int status = 0;
    (void)::waitpid(child, &status, 0);
    expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "accept EINTR helper must connect and exit cleanly");
    (void)::sigaction(SIGUSR2, &old_action, nullptr);
}

void test_listener_accept_drain_and_flags() {
    hp::net::Socket listener = hp::net::Socket::create_tcp();
    expect(is_non_blocking(listener.fd()), "listener must be non-blocking");
    expect(is_close_on_exec(listener.fd()),
           "listener must be close-on-exec");
    listener.set_reuse_address(true);
    listener.bind_any(0);
    listener.listen(16);
    const std::uint16_t port = listener.local_port();
    expect(port != 0, "port 0 bind must report an actual non-zero port");

    std::vector<int> clients;
    for (int index = 0; index < 4; ++index) {
        const int client = connect_loopback(port);
        expect(client >= 0, "loopback client must connect");
        if (client >= 0) {
            clients.push_back(client);
        }
    }

    std::vector<hp::net::Socket> accepted;
    for (int readiness = 0;
         readiness < 8 && accepted.size() < clients.size(); ++readiness) {
        pollfd descriptor{listener.fd(), POLLIN, 0};
        const int poll_result = ::poll(&descriptor, 1, 250);
        if (poll_result == -1 && errno == EINTR) {
            --readiness;
            continue;
        }
        expect(poll_result == 1 && (descriptor.revents & POLLIN) != 0,
               "listener must become readable for queued connections");
        if (poll_result != 1) {
            continue;
        }

        while (true) {
            hp::net::Socket connection = listener.accept_non_blocking();
            if (!connection.valid()) {
                break;
            }
            expect(is_non_blocking(connection.fd()),
                   "accepted connection must be non-blocking");
            expect(is_close_on_exec(connection.fd()),
                   "accepted connection must be close-on-exec");
            accepted.push_back(std::move(connection));
        }
    }
    expect(accepted.size() == clients.size(),
           "readiness-driven accept drains must consume every queued connection");
    expect(!listener.accept_non_blocking().valid(),
           "drained listener must keep reporting EAGAIN as invalid Socket");

    for (int client : clients) {
        ::close(client);
    }
}

void test_epoller_invalid_operation() {
    hp::net::Epoller epoller;
    try {
        epoller.add(-1, EPOLLIN, 1);
        expect(false, "invalid epoll add must throw");
    } catch (const std::system_error& error) {
        expect(error.code().value() == EBADF,
               "invalid epoll add must preserve EBADF");
        expect(std::string(error.what()).find("epoll_ctl") != std::string::npos,
               "invalid epoll add must name epoll_ctl");
    }
}

void test_registration_failure_preserves_unique_ownership() {
    int fds[2] = {-1, -1};
    expect(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                        fds) == 0,
           "registration-failure socketpair must be created");
    if (fds[0] == -1) {
        return;
    }

    hp::net::Socket sole_owner(fds[0]);
    hp::net::Epoller epoller;
    epoller.add(sole_owner.fd(), EPOLLIN, 10);
    try {
        epoller.add(sole_owner.fd(), EPOLLIN, 11);
        expect(false, "duplicate epoll registration must fail");
    } catch (const std::system_error& error) {
        expect(error.code().value() == EEXIST,
               "duplicate registration must preserve EEXIST");
    }
    expect(::fcntl(sole_owner.fd(), F_GETFD) != -1,
           "registration failure must not close the sole Socket owner");
    epoller.remove(sole_owner.fd());
    const int formerly_owned_fd = sole_owner.fd();
    sole_owner.reset();
    errno = 0;
    expect(::fcntl(formerly_owned_fd, F_GETFD) == -1 && errno == EBADF,
           "Socket destruction path must release the failed-registration fd");
    ::close(fds[1]);
}

void test_combined_read_and_half_close_event() {
    int fds[2] = {-1, -1};
    expect(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                        fds) == 0,
           "combined-event socketpair must be created");
    if (fds[0] == -1) {
        return;
    }

    hp::net::Epoller epoller;
    constexpr std::uint64_t token = 0xabcdefULL;
    epoller.add(fds[0], EPOLLIN | EPOLLRDHUP, token);
    const char payload[] = {'a', '\0', 'z'};
    expect(::send(fds[1], payload, sizeof(payload), MSG_NOSIGNAL) ==
               static_cast<ssize_t>(sizeof(payload)),
           "combined-event payload must be sent");
    expect(::shutdown(fds[1], SHUT_WR) == 0,
           "combined-event peer must half-close writes");

    std::uint32_t observed = 0;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const auto events = epoller.wait(250);
        for (const epoll_event& event : events) {
            if (event.data.u64 == token) {
                observed |= event.events;
            }
        }
        if ((observed & (EPOLLIN | EPOLLRDHUP)) ==
            (EPOLLIN | EPOLLRDHUP)) {
            break;
        }
    }
    expect((observed & EPOLLIN) != 0U,
           "combined close event must retain readable bytes");
    expect((observed & EPOLLRDHUP) != 0U,
           "combined close event must retain peer half-close state");

    char received[sizeof(payload)]{};
    expect(::recv(fds[0], received, sizeof(received), 0) ==
               static_cast<ssize_t>(sizeof(received)),
           "readable bytes must remain available after combined observation");
    expect(std::equal(std::begin(payload), std::end(payload),
                      std::begin(received)),
           "combined event must not alter readable binary bytes");
    epoller.remove(fds[0]);
    ::close(fds[0]);
    ::close(fds[1]);
}

}  // namespace

int main() {
    test_epoller_lifecycle_and_lt();
    test_epoll_wait_retries_eintr();
    test_accept_retries_eintr();
    test_listener_accept_drain_and_flags();
    test_epoller_invalid_operation();
    test_registration_failure_preserves_unique_ownership();
    test_combined_read_and_half_close_event();

    if (failures != 0) {
        std::cerr << failures << " network primitive assertion(s) failed\n";
        return 1;
    }
    std::cout << "Network primitive tests passed\n";
    return 0;
}
