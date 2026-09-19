#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#include "net/epoller.h"
#include "net/socket.h"

namespace {

int failures = 0;

void Expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool IsNonBlocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL);
  return flags != -1 && (flags & O_NONBLOCK) != 0;
}

bool IsCloseOnExec(int fd) {
  const int flags = ::fcntl(fd, F_GETFD);
  return flags != -1 && (flags & FD_CLOEXEC) != 0;
}

void TestEpollerLifecycleAndLt() {
  hp::net::Epoller epoller(4);
  Expect(IsCloseOnExec(epoller.fd()),
         "epoll fd must have close-on-exec semantics");

  int pipe_fds[2] = {-1, -1};
  Expect(::pipe2(pipe_fds, O_NONBLOCK | O_CLOEXEC) == 0,
         "pipe2 must succeed for LT test");
  if (pipe_fds[0] == -1) {
    return;
  }

  constexpr std::uint64_t kToken = 0x12345678ULL;
  epoller.Add(pipe_fds[0], EPOLLIN, kToken);
  const char byte = 'x';
  Expect(::write(pipe_fds[1], &byte, 1) == 1, "LT test byte must be written");

  const auto first = epoller.Wait(0);
  Expect(first.size() == 1, "first LT wait must observe unread data");
  if (!first.empty()) {
    Expect(first.front().data.u64 == kToken,
           "epoll must preserve caller token");
    Expect((first.front().events & EPOLLIN) != 0U,
           "first LT event must be readable");
  }

  const auto second = epoller.Wait(0);
  Expect(second.size() == 1,
         "LT must report the same unread level on a second wait");

  epoller.Modify(pipe_fds[0], EPOLLIN, kToken + 1);
  const auto modified = epoller.Wait(0);
  Expect(modified.size() == 1 && modified.front().data.u64 == kToken + 1,
         "modify must replace the observation token");

  char received = 0;
  Expect(::read(pipe_fds[0], &received, 1) == 1,
         "LT test byte must be drained");
  Expect(epoller.Wait(0).empty(), "drained level must no longer be ready");

  epoller.Remove(pipe_fds[0]);
  epoller.Remove(pipe_fds[0]);
  ::close(pipe_fds[0]);
  ::close(pipe_fds[1]);
}

volatile std::sig_atomic_t signal_count = 0;

extern "C" void CountSignal(int) { signal_count = 1; }

void TestEpollWaitRetriesEintr() {
  struct sigaction action {};

  action.sa_handler = CountSignal;
  ::sigemptyset(&action.sa_mask);
  action.sa_flags = 0;

  struct sigaction old_action {};

  Expect(::sigaction(SIGALRM, &action, &old_action) == 0,
         "SIGALRM handler must install");

  hp::net::Epoller epoller;
  ::ualarm(10'000, 0);
  const auto events = epoller.Wait(30);
  ::ualarm(0, 0);

  Expect(events.empty(), "EINTR retry wait must eventually time out empty");
  Expect(signal_count > 0, "controlled signal must interrupt epoll_wait");
  Expect(::sigaction(SIGALRM, &old_action, nullptr) == 0,
         "SIGALRM handler must restore");
}

int ConnectLoopback(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd == -1) {
    return -1;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == -1) {
    ::close(fd);
    return -1;
  }
  return fd;
}

void TestAcceptRetriesEintr() {
  const int raw_listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  Expect(raw_listener >= 0, "blocking listener socket must be created");
  if (raw_listener < 0) {
    return;
  }
  hp::net::Socket listener(raw_listener);
  listener.set_reuse_address(true);
  listener.BindAny(0);
  listener.Listen(4);

  struct sigaction action {};

  action.sa_handler = CountSignal;
  ::sigemptyset(&action.sa_mask);
  action.sa_flags = 0;

  struct sigaction old_action {};

  Expect(::sigaction(SIGUSR2, &action, &old_action) == 0,
         "SIGUSR2 handler must install");
  signal_count = 0;

  const pid_t child = ::fork();
  Expect(child >= 0, "accept EINTR helper must fork");
  if (child == 0) {
    ::usleep(20'000);
    ::kill(::getppid(), SIGUSR2);
    ::usleep(20'000);
    const int client = ConnectLoopback(listener.local_port());
    if (client >= 0) {
      ::close(client);
    }
    _exit(client >= 0 ? 0 : 1);
  }
  if (child < 0) {
    (void)::sigaction(SIGUSR2, &old_action, nullptr);
    return;
  }

  hp::net::Socket accepted = listener.AcceptNonBlocking();
  Expect(signal_count > 0, "controlled signal must interrupt blocking accept4");
  Expect(accepted.valid(), "accept4 must retry EINTR and accept the client");
  if (accepted.valid()) {
    Expect(IsNonBlocking(accepted.fd()),
           "EINTR-retried accepted fd must be non-blocking");
    Expect(IsCloseOnExec(accepted.fd()),
           "EINTR-retried accepted fd must be close-on-exec");
  }
  int status = 0;
  (void)::waitpid(child, &status, 0);
  Expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "accept EINTR helper must connect and exit cleanly");
  (void)::sigaction(SIGUSR2, &old_action, nullptr);
}

void TestListenerAcceptDrainAndFlags() {
  hp::net::Socket listener = hp::net::Socket::CreateTcp();
  Expect(IsNonBlocking(listener.fd()), "listener must be non-blocking");
  Expect(IsCloseOnExec(listener.fd()), "listener must be close-on-exec");
  listener.set_reuse_address(true);
  listener.BindAny(0);
  listener.Listen(16);
  const std::uint16_t port = listener.local_port();
  Expect(port != 0, "port 0 bind must report an actual non-zero port");

  std::vector<int> clients;
  for (int index = 0; index < 4; ++index) {
    const int client = ConnectLoopback(port);
    Expect(client >= 0, "loopback client must connect");
    if (client >= 0) {
      clients.push_back(client);
    }
  }

  std::vector<hp::net::Socket> accepted;
  for (int readiness = 0; readiness < 8 && accepted.size() < clients.size();
       ++readiness) {
    pollfd descriptor{listener.fd(), POLLIN, 0};
    const int poll_result = ::poll(&descriptor, 1, 250);
    if (poll_result == -1 && errno == EINTR) {
      --readiness;
      continue;
    }
    Expect(poll_result == 1 && (descriptor.revents & POLLIN) != 0,
           "listener must become readable for queued connections");
    if (poll_result != 1) {
      continue;
    }

    while (true) {
      hp::net::Socket connection = listener.AcceptNonBlocking();
      if (!connection.valid()) {
        break;
      }
      Expect(IsNonBlocking(connection.fd()),
             "accepted connection must be non-blocking");
      Expect(IsCloseOnExec(connection.fd()),
             "accepted connection must be close-on-exec");
      accepted.push_back(std::move(connection));
    }
  }
  Expect(accepted.size() == clients.size(),
         "readiness-driven accept drains must consume every queued connection");
  Expect(!listener.AcceptNonBlocking().valid(),
         "drained listener must keep reporting EAGAIN as invalid Socket");

  for (int client : clients) {
    ::close(client);
  }
}

void TestEpollerInvalidOperation() {
  hp::net::Epoller epoller;
  try {
    epoller.Add(-1, EPOLLIN, 1);
    Expect(false, "invalid epoll add must throw");
  } catch (const std::system_error& error) {
    Expect(error.code().value() == EBADF,
           "invalid epoll add must preserve EBADF");
    Expect(std::string(error.what()).find("epoll_ctl") != std::string::npos,
           "invalid epoll add must name epoll_ctl");
  }
}

void TestRegistrationFailurePreservesUniqueOwnership() {
  int fds[2] = {-1, -1};
  Expect(::socketpair(AF_UNIX,
                      SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                      0,
                      fds) == 0,
         "registration-failure socketpair must be created");
  if (fds[0] == -1) {
    return;
  }

  hp::net::Socket sole_owner(fds[0]);
  hp::net::Epoller epoller;
  epoller.Add(sole_owner.fd(), EPOLLIN, 10);
  try {
    epoller.Add(sole_owner.fd(), EPOLLIN, 11);
    Expect(false, "duplicate epoll registration must fail");
  } catch (const std::system_error& error) {
    Expect(error.code().value() == EEXIST,
           "duplicate registration must preserve EEXIST");
  }
  Expect(::fcntl(sole_owner.fd(), F_GETFD) != -1,
         "registration failure must not close the sole Socket owner");
  epoller.Remove(sole_owner.fd());
  const int formerly_owned_fd = sole_owner.fd();
  sole_owner.Reset();
  errno = 0;
  Expect(::fcntl(formerly_owned_fd, F_GETFD) == -1 && errno == EBADF,
         "Socket destruction path must release the failed-registration fd");
  ::close(fds[1]);
}

void TestCombinedReadAndHalfCloseEvent() {
  int fds[2] = {-1, -1};
  Expect(::socketpair(AF_UNIX,
                      SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                      0,
                      fds) == 0,
         "combined-event socketpair must be created");
  if (fds[0] == -1) {
    return;
  }

  hp::net::Epoller epoller;
  constexpr std::uint64_t kToken = 0xabcdefULL;
  epoller.Add(fds[0], EPOLLIN | EPOLLRDHUP, kToken);
  const char payload[] = {'a', '\0', 'z'};
  Expect(::send(fds[1], payload, sizeof(payload), MSG_NOSIGNAL) ==
             static_cast<ssize_t>(sizeof(payload)),
         "combined-event payload must be sent");
  Expect(::shutdown(fds[1], SHUT_WR) == 0,
         "combined-event peer must half-close writes");

  std::uint32_t observed = 0;
  for (int attempt = 0; attempt < 4; ++attempt) {
    const auto events = epoller.Wait(250);
    for (const epoll_event& event : events) {
      if (event.data.u64 == kToken) {
        observed |= event.events;
      }
    }
    if ((observed & (EPOLLIN | EPOLLRDHUP)) == (EPOLLIN | EPOLLRDHUP)) {
      break;
    }
  }
  Expect((observed & EPOLLIN) != 0U,
         "combined close event must retain readable bytes");
  Expect((observed & EPOLLRDHUP) != 0U,
         "combined close event must retain peer half-close state");

  char received[sizeof(payload)]{};
  Expect(::recv(fds[0], received, sizeof(received), 0) ==
             static_cast<ssize_t>(sizeof(received)),
         "readable bytes must remain available after combined observation");
  Expect(
      std::equal(std::begin(payload), std::end(payload), std::begin(received)),
      "combined event must not alter readable binary bytes");
  epoller.Remove(fds[0]);
  ::close(fds[0]);
  ::close(fds[1]);
}

}  // namespace

int main() {
  TestEpollerLifecycleAndLt();
  TestEpollWaitRetriesEintr();
  TestAcceptRetriesEintr();
  TestListenerAcceptDrainAndFlags();
  TestEpollerInvalidOperation();
  TestRegistrationFailurePreservesUniqueOwnership();
  TestCombinedReadAndHalfCloseEvent();

  if (failures != 0) {
    std::cerr << failures << " network primitive assertion(s) failed\n";
    return 1;
  }
  std::cout << "Network primitive tests passed\n";
  return 0;
}
