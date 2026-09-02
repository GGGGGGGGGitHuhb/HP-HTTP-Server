#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::vector<std::byte> make_payload(std::size_t size, std::uint32_t seed) {
    std::vector<std::byte> payload(size);
    for (std::size_t index = 0; index < size; ++index) {
        payload[index] =
            static_cast<std::byte>((index * 193U + seed * 17U) & 0xffU);
    }
    return payload;
}

class ServerProcess {
public:
    ServerProcess(pid_t pid, int output_fd, std::uint16_t port,
                  std::string startup_output)
        : pid_(pid),
          output_fd_(output_fd),
          port_(port),
          startup_output_(std::move(startup_output)) {}

    ~ServerProcess() {
        stop();
    }

    ServerProcess(const ServerProcess&) = delete;
    ServerProcess& operator=(const ServerProcess&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }

    [[nodiscard]] const std::string& startup_output() const noexcept {
        return startup_output_;
    }

    [[nodiscard]] bool running() const noexcept {
        return pid_ > 0 && ::kill(pid_, 0) == 0;
    }

    void stop() noexcept {
        if (pid_ > 0) {
            (void)::kill(pid_, SIGTERM);
            int status = 0;
            while (::waitpid(pid_, &status, 0) == -1 && errno == EINTR) {
            }
            pid_ = -1;
        }
        if (output_fd_ >= 0) {
            ::close(output_fd_);
            output_fd_ = -1;
        }
    }

private:
    pid_t pid_{-1};
    int output_fd_{-1};
    std::uint16_t port_{0};
    std::string startup_output_;
};

ServerProcess start_server(const char* executable) {
    int output_pipe[2] = {-1, -1};
    if (::pipe2(output_pipe, O_CLOEXEC) == -1) {
        throw std::runtime_error(std::string("pipe2: ") + std::strerror(errno));
    }

    const pid_t child = ::fork();
    if (child == -1) {
        const int error_number = errno;
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
        throw std::runtime_error(std::string("fork: ") +
                                 std::strerror(error_number));
    }
    if (child == 0) {
        ::close(output_pipe[0]);
        if (::dup2(output_pipe[1], STDOUT_FILENO) == -1 ||
            ::dup2(output_pipe[1], STDERR_FILENO) == -1) {
            _exit(126);
        }
        ::close(output_pipe[1]);
        char* const arguments[] = {const_cast<char*>(executable),
                                   const_cast<char*>("--port"),
                                   const_cast<char*>("0"), nullptr};
        ::execv(executable, arguments);
        _exit(127);
    }

    ::close(output_pipe[1]);
    const int flags = ::fcntl(output_pipe[0], F_GETFL);
    if (flags == -1 ||
        ::fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK) == -1) {
        const int error_number = errno;
        ::kill(child, SIGKILL);
        (void)::waitpid(child, nullptr, 0);
        ::close(output_pipe[0]);
        throw std::runtime_error(std::string("fcntl(output pipe): ") +
                                 std::strerror(error_number));
    }

    std::string output;
    constexpr std::string_view marker =
        "V0.1 / S2 TCP echo listening on port ";
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd descriptor{output_pipe[0], POLLIN, 0};
        const int poll_result = ::poll(&descriptor, 1, 100);
        if (poll_result == -1 && errno == EINTR) {
            continue;
        }
        if (poll_result > 0 && (descriptor.revents & POLLIN) != 0) {
            char buffer[1024];
            while (true) {
                const ssize_t count =
                    ::read(output_pipe[0], buffer, sizeof(buffer));
                if (count > 0) {
                    output.append(buffer, static_cast<std::size_t>(count));
                    continue;
                }
                if (count == -1 && errno == EINTR) {
                    continue;
                }
                break;
            }
        }

        const auto marker_position = output.find(marker);
        if (marker_position != std::string::npos) {
            const std::size_t port_start = marker_position + marker.size();
            const std::size_t port_end = output.find_first_not_of("0123456789",
                                                                  port_start);
            const auto port_text = output.substr(port_start, port_end - port_start);
            const unsigned long parsed = std::stoul(port_text);
            if (parsed > 0 && parsed <= 65535) {
                return ServerProcess(child, output_pipe[0],
                                     static_cast<std::uint16_t>(parsed), output);
            }
        }

        int status = 0;
        if (::waitpid(child, &status, WNOHANG) == child) {
            ::close(output_pipe[0]);
            throw std::runtime_error("server exited during startup: " + output);
        }
    }

    ::kill(child, SIGKILL);
    (void)::waitpid(child, nullptr, 0);
    ::close(output_pipe[0]);
    throw std::runtime_error("timed out waiting for startup output: " + output);
}

int connect_client(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd == -1) {
        throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
    }
    timeval timeout{3, 0};
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                  sizeof(address)) == -1) {
        const int error_number = errno;
        ::close(fd);
        throw std::runtime_error(std::string("connect: ") +
                                 std::strerror(error_number));
    }
    return fd;
}

void send_all(int fd, const std::vector<std::byte>& payload) {
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const ssize_t count =
            ::send(fd, payload.data() + offset, payload.size() - offset,
                   MSG_NOSIGNAL);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count == -1 && errno == EINTR) {
            continue;
        }
        throw std::runtime_error(std::string("send: ") + std::strerror(errno));
    }
}

std::vector<std::byte> receive_exact(int fd, std::size_t size) {
    std::vector<std::byte> received(size);
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t count =
            ::recv(fd, received.data() + offset, size - offset, 0);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count == -1 && errno == EINTR) {
            continue;
        }
        throw std::runtime_error(count == 0 ? "unexpected EOF" :
                                 std::string("recv: ") + std::strerror(errno));
    }
    return received;
}

void echo_round_trip(std::uint16_t port, const std::vector<std::byte>& payload) {
    const int client = connect_client(port);
    send_all(client, payload);
    const auto received = receive_exact(client, payload.size());
    expect(received == payload,
           "loopback echo must preserve order, length and binary bytes");
    ::close(client);
}

void test_text_binary_and_multi_read(std::uint16_t port) {
    const std::string text = "single-thread epoll LT echo";
    const std::vector<std::byte> text_payload(
        reinterpret_cast<const std::byte*>(text.data()),
        reinterpret_cast<const std::byte*>(text.data() + text.size()));
    echo_round_trip(port, text_payload);
    echo_round_trip(port, make_payload(257, 1));
    echo_round_trip(port, make_payload(256 * 1024 + 73, 2));
}

void test_accept_drain_and_multiple_connections(std::uint16_t port) {
    std::vector<int> clients;
    for (int index = 0; index < 8; ++index) {
        clients.push_back(connect_client(port));
    }
    for (std::size_t index = 0; index < clients.size(); ++index) {
        const auto payload = make_payload(1024 + index * 31, index + 3);
        send_all(clients[index], payload);
        const auto received = receive_exact(clients[index], payload.size());
        expect(received == payload,
               "each queued connection must receive only its own echo");
    }
    for (int client : clients) {
        ::close(client);
    }
}

void test_half_close_drains_then_eof(std::uint16_t port) {
    const int client = connect_client(port);
    const auto payload = make_payload(192 * 1024 + 11, 9);
    send_all(client, payload);
    expect(::shutdown(client, SHUT_WR) == 0,
           "client write half-close must succeed");
    const auto received = receive_exact(client, payload.size());
    expect(received == payload,
           "half-close must not truncate buffered echo output");
    std::byte extra{};
    ssize_t count = -1;
    do {
        count = ::recv(client, &extra, 1, 0);
    } while (count == -1 && errno == EINTR);
    expect(count == 0, "server must close after half-close output drains");
    ::close(client);
}

void test_reset_isolation_and_fd_reuse(std::uint16_t port) {
    const int reset_client = connect_client(port);
    linger reset_linger{1, 0};
    (void)::setsockopt(reset_client, SOL_SOCKET, SO_LINGER, &reset_linger,
                       sizeof(reset_linger));
    const auto discarded = make_payload(4096, 5);
    send_all(reset_client, discarded);
    ::close(reset_client);

    for (std::uint32_t attempt = 0; attempt < 40; ++attempt) {
        echo_round_trip(port, make_payload(64 + attempt, attempt + 20));
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: server_integration_tests <hp_http_server-path>\n";
        return 2;
    }

    try {
        ServerProcess server = start_server(argv[1]);
        expect(server.port() != 0, "--port 0 must report an actual port");
        expect(server.startup_output().find("V0.1 / S2 TCP echo") !=
                   std::string::npos,
               "startup must identify the temporary S2 echo capability");
        expect(server.startup_output().find("HTTP service is not available") !=
                   std::string::npos,
               "startup must explicitly deny HTTP availability");

        test_text_binary_and_multi_read(server.port());
        test_accept_drain_and_multiple_connections(server.port());
        test_half_close_drains_then_eof(server.port());
        test_reset_isolation_and_fd_reuse(server.port());
        expect(server.running(),
               "connection reset and repeated fd reuse must not stop server");
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }

    if (failures != 0) {
        std::cerr << failures << " server integration assertion(s) failed\n";
        return 1;
    }
    std::cout << "Server integration tests passed\n";
    return 0;
}
