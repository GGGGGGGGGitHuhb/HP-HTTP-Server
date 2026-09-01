#include "net/socket.h"

#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <system_error>
#include <sys/socket.h>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool create_pipe(int (&fds)[2], const std::string& purpose) {
    fds[0] = -1;
    fds[1] = -1;
    const bool created = ::pipe(fds) == 0;
    expect(created, "pipe creation failed for " + purpose);
    return created;
}

bool is_open(int fd) {
    errno = 0;
    return ::fcntl(fd, F_GETFD) != -1;
}

bool is_closed(int fd) {
    errno = 0;
    return ::fcntl(fd, F_GETFD) == -1 && errno == EBADF;
}

void close_fd(int fd) {
    if (fd >= 0) {
        ::close(fd);
    }
}

void test_default_state() {
    hp::net::Socket socket;
    expect(!socket.valid(), "default socket must be invalid");
    expect(socket.fd() == -1, "default fd must be -1");
    expect(socket.release() == -1, "release on invalid socket must return -1");
    socket.reset();
}

void test_destructor() {
    int fds[2];
    if (!create_pipe(fds, "destructor test")) {
        return;
    }

    const int owned_fd = fds[0];
    {
        hp::net::Socket socket(owned_fd);
        expect(socket.valid(), "socket must own the pipe fd");
    }
    expect(is_closed(owned_fd), "destructor must close its owned fd");
    close_fd(fds[1]);
}

void test_move_constructor() {
    int fds[2];
    if (!create_pipe(fds, "move constructor test")) {
        return;
    }

    const int owned_fd = fds[0];
    {
        hp::net::Socket source(owned_fd);
        hp::net::Socket destination(std::move(source));
        expect(!source.valid(), "move source must become invalid");
        expect(destination.fd() == owned_fd, "destination must own source fd");
        expect(is_open(owned_fd), "moved fd must remain open");
    }
    expect(is_closed(owned_fd), "moved fd must close with destination");
    close_fd(fds[1]);
}

void test_move_assignment() {
    int target_pipe[2];
    int source_pipe[2];
    if (!create_pipe(target_pipe, "move assignment target")) {
        return;
    }
    if (!create_pipe(source_pipe, "move assignment source")) {
        close_fd(target_pipe[0]);
        close_fd(target_pipe[1]);
        return;
    }

    const int old_target_fd = target_pipe[0];
    const int source_fd = source_pipe[0];
    {
        hp::net::Socket target(old_target_fd);
        hp::net::Socket source(source_fd);
        target = std::move(source);
        expect(!source.valid(), "move-assigned source must become invalid");
        expect(is_closed(old_target_fd), "assignment must close old target fd");
        expect(target.fd() == source_fd, "target must own source fd");
        expect(is_open(source_fd), "new target fd must remain open");
    }
    expect(is_closed(source_fd), "source fd must close with target");
    close_fd(target_pipe[1]);
    close_fd(source_pipe[1]);
}

void test_release() {
    int fds[2];
    if (!create_pipe(fds, "release test")) {
        return;
    }

    int released_fd = -1;
    {
        hp::net::Socket socket(fds[0]);
        released_fd = socket.release();
        expect(!socket.valid(), "release must invalidate socket");
        expect(released_fd == fds[0], "release must return owned fd");
    }
    expect(is_open(released_fd), "released fd must outlive former owner");
    close_fd(released_fd);
    close_fd(fds[1]);
}

void test_reset() {
    int first_pipe[2];
    int second_pipe[2];
    if (!create_pipe(first_pipe, "reset old fd")) {
        return;
    }
    if (!create_pipe(second_pipe, "reset new fd")) {
        close_fd(first_pipe[0]);
        close_fd(first_pipe[1]);
        return;
    }

    const int old_fd = first_pipe[0];
    const int new_fd = second_pipe[0];
    hp::net::Socket socket(old_fd);
    socket.reset(new_fd);
    expect(is_closed(old_fd), "reset must close the old fd");
    expect(socket.fd() == new_fd, "reset must own the new fd");
    expect(is_open(new_fd), "new fd must remain open");

    socket.reset(new_fd);
    expect(socket.fd() == new_fd, "same-fd reset must preserve ownership");
    expect(is_open(new_fd), "same-fd reset must not close the fd");

    socket.reset();
    expect(!socket.valid(), "empty reset must invalidate socket");
    expect(is_closed(new_fd), "empty reset must close the owned fd");
    close_fd(first_pipe[1]);
    close_fd(second_pipe[1]);
}

void test_non_blocking() {
    int fds[2];
    if (!create_pipe(fds, "non-blocking test")) {
        return;
    }

    hp::net::Socket socket(fds[0]);
    socket.set_non_blocking();
    const int flags = ::fcntl(socket.fd(), F_GETFL);
    expect(flags != -1, "non-blocking fd flags must be readable");
    expect((flags & O_NONBLOCK) != 0, "O_NONBLOCK must be enabled");
    close_fd(fds[1]);
}

void test_reuse_address() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    expect(fd >= 0, "AF_INET socket creation must succeed");
    if (fd < 0) {
        return;
    }

    hp::net::Socket socket(fd);
    socket.set_reuse_address(true);

    int option = 0;
    socklen_t option_length = sizeof(option);
    expect(::getsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &option,
                        &option_length) == 0,
           "SO_REUSEADDR must be readable");
    expect(option != 0, "SO_REUSEADDR must be enabled");

    socket.set_reuse_address(false);
    option = 1;
    expect(::getsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &option,
                        &option_length) == 0,
           "SO_REUSEADDR must remain readable");
    expect(option == 0, "SO_REUSEADDR must be disabled");
}

void test_invalid_fd_failures() {
    hp::net::Socket socket;

    try {
        socket.set_non_blocking();
        expect(false, "invalid non-blocking operation must throw");
    } catch (const std::system_error& error) {
        expect(error.code().value() == EBADF,
               "non-blocking failure must preserve EBADF");
        expect(std::string(error.what()).find("fcntl(F_GETFL)") !=
                   std::string::npos,
               "non-blocking error must name its operation");
    }

    try {
        socket.set_reuse_address(true);
        expect(false, "invalid reuse-address operation must throw");
    } catch (const std::system_error& error) {
        expect(error.code().value() == EBADF,
               "reuse-address failure must preserve EBADF");
        expect(std::string(error.what()).find("setsockopt(SO_REUSEADDR)") !=
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
    test_default_state();
    test_destructor();
    test_move_constructor();
    test_move_assignment();
    test_release();
    test_reset();
    test_non_blocking();
    test_reuse_address();
    test_invalid_fd_failures();

    if (failures != 0) {
        std::cerr << failures << " socket test assertion(s) failed\n";
        return 1;
    }
    std::cout << "Socket tests passed\n";
    return 0;
}
