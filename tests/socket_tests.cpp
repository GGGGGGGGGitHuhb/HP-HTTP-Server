#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <iostream>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

#include "net/socket.h"

namespace {

int failures = 0;

void Expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool CreatePipe(int (&fds)[2], const std::string& purpose) {
  fds[0] = -1;
  fds[1] = -1;
  const bool created = ::pipe(fds) == 0;
  Expect(created, "pipe creation failed for " + purpose);
  return created;
}

bool IsOpen(int fd) {
  errno = 0;
  return ::fcntl(fd, F_GETFD) != -1;
}

bool IsClosed(int fd) {
  errno = 0;
  return ::fcntl(fd, F_GETFD) == -1 && errno == EBADF;
}

void CloseFd(int fd) {
  if (fd >= 0) {
    ::close(fd);
  }
}

void TestDefaultState() {
  hp::net::Socket socket;
  Expect(!socket.valid(), "default socket must be invalid");
  Expect(socket.fd() == -1, "default fd must be -1");
  Expect(socket.Release() == -1, "release on invalid socket must return -1");
  socket.Reset();
}

void TestDestructor() {
  int fds[2];
  if (!CreatePipe(fds, "destructor test")) {
    return;
  }

  const int owned_fd = fds[0];
  {
    hp::net::Socket socket(owned_fd);
    Expect(socket.valid(), "socket must own the pipe fd");
  }
  Expect(IsClosed(owned_fd), "destructor must close its owned fd");
  CloseFd(fds[1]);
}

void TestMoveConstructor() {
  int fds[2];
  if (!CreatePipe(fds, "move constructor test")) {
    return;
  }

  const int owned_fd = fds[0];
  {
    hp::net::Socket source(owned_fd);
    hp::net::Socket destination(std::move(source));
    Expect(!source.valid(), "move source must become invalid");
    Expect(destination.fd() == owned_fd, "destination must own source fd");
    Expect(IsOpen(owned_fd), "moved fd must remain open");
  }
  Expect(IsClosed(owned_fd), "moved fd must close with destination");
  CloseFd(fds[1]);
}

void TestMoveAssignment() {
  int target_pipe[2];
  int source_pipe[2];
  if (!CreatePipe(target_pipe, "move assignment target")) {
    return;
  }
  if (!CreatePipe(source_pipe, "move assignment source")) {
    CloseFd(target_pipe[0]);
    CloseFd(target_pipe[1]);
    return;
  }

  const int old_target_fd = target_pipe[0];
  const int source_fd = source_pipe[0];
  {
    hp::net::Socket target(old_target_fd);
    hp::net::Socket source(source_fd);
    target = std::move(source);
    Expect(!source.valid(), "move-assigned source must become invalid");
    Expect(IsClosed(old_target_fd), "assignment must close old target fd");
    Expect(target.fd() == source_fd, "target must own source fd");
    Expect(IsOpen(source_fd), "new target fd must remain open");
  }
  Expect(IsClosed(source_fd), "source fd must close with target");
  CloseFd(target_pipe[1]);
  CloseFd(source_pipe[1]);
}

void TestRelease() {
  int fds[2];
  if (!CreatePipe(fds, "release test")) {
    return;
  }

  int released_fd = -1;
  {
    hp::net::Socket socket(fds[0]);
    released_fd = socket.Release();
    Expect(!socket.valid(), "release must invalidate socket");
    Expect(released_fd == fds[0], "release must return owned fd");
  }
  Expect(IsOpen(released_fd), "released fd must outlive former owner");
  CloseFd(released_fd);
  CloseFd(fds[1]);
}

void TestReset() {
  int first_pipe[2];
  int second_pipe[2];
  if (!CreatePipe(first_pipe, "reset old fd")) {
    return;
  }
  if (!CreatePipe(second_pipe, "reset new fd")) {
    CloseFd(first_pipe[0]);
    CloseFd(first_pipe[1]);
    return;
  }

  const int old_fd = first_pipe[0];
  const int new_fd = second_pipe[0];
  hp::net::Socket socket(old_fd);
  socket.Reset(new_fd);
  Expect(IsClosed(old_fd), "reset must close the old fd");
  Expect(socket.fd() == new_fd, "reset must own the new fd");
  Expect(IsOpen(new_fd), "new fd must remain open");

  socket.Reset(new_fd);
  Expect(socket.fd() == new_fd, "same-fd reset must preserve ownership");
  Expect(IsOpen(new_fd), "same-fd reset must not close the fd");

  socket.Reset();
  Expect(!socket.valid(), "empty reset must invalidate socket");
  Expect(IsClosed(new_fd), "empty reset must close the owned fd");
  CloseFd(first_pipe[1]);
  CloseFd(second_pipe[1]);
}

void TestNonBlocking() {
  int fds[2];
  if (!CreatePipe(fds, "non-blocking test")) {
    return;
  }

  hp::net::Socket socket(fds[0]);
  socket.set_non_blocking();
  const int flags = ::fcntl(socket.fd(), F_GETFL);
  Expect(flags != -1, "non-blocking fd flags must be readable");
  Expect((flags & O_NONBLOCK) != 0, "O_NONBLOCK must be enabled");
  CloseFd(fds[1]);
}

void TestReuseAddress() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  Expect(fd >= 0, "AF_INET socket creation must succeed");
  if (fd < 0) {
    return;
  }

  hp::net::Socket socket(fd);
  socket.set_reuse_address(true);

  int option = 0;
  socklen_t option_length = sizeof(option);
  Expect(
      ::getsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &option, &option_length) == 0,
      "SO_REUSEADDR must be readable");
  Expect(option != 0, "SO_REUSEADDR must be enabled");

  socket.set_reuse_address(false);
  option = 1;
  Expect(
      ::getsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &option, &option_length) == 0,
      "SO_REUSEADDR must remain readable");
  Expect(option == 0, "SO_REUSEADDR must be disabled");
}

void TestInvalidFdFailures() {
  hp::net::Socket socket;

  try {
    socket.set_non_blocking();
    Expect(false, "invalid non-blocking operation must throw");
  } catch (const std::system_error& error) {
    Expect(error.code().value() == EBADF,
           "non-blocking failure must preserve EBADF");
    Expect(
        std::string(error.what()).find("fcntl(F_GETFL)") != std::string::npos,
        "non-blocking error must name its operation");
  }

  try {
    socket.set_reuse_address(true);
    Expect(false, "invalid reuse-address operation must throw");
  } catch (const std::system_error& error) {
    Expect(error.code().value() == EBADF,
           "reuse-address failure must preserve EBADF");
    Expect(std::string(error.what()).find("setsockopt(SO_REUSEADDR)") !=
               std::string::npos,
           "reuse-address error must name its operation");
  }
}

static_assert(!std::is_copy_constructible_v<hp::net::Socket>);
static_assert(!std::is_copy_assignable_v<hp::net::Socket>);
static_assert(std::is_nothrow_move_constructible_v<hp::net::Socket>);
static_assert(std::is_nothrow_move_assignable_v<hp::net::Socket>);
static_assert(std::is_nothrow_destructible_v<hp::net::Socket>);

}  // namespace

int main() {
  TestDefaultState();
  TestDestructor();
  TestMoveConstructor();
  TestMoveAssignment();
  TestRelease();
  TestReset();
  TestNonBlocking();
  TestReuseAddress();
  TestInvalidFdFailures();

  if (failures != 0) {
    std::cerr << failures << " socket test assertion(s) failed\n";
    return 1;
  }
  std::cout << "Socket tests passed\n";
  return 0;
}
