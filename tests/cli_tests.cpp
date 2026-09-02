#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

struct RunResult {
    int exit_code;
    std::string output;
};

RunResult run_process(const char* executable,
                      const std::vector<std::string>& arguments) {
    int output_pipe[2];
    if (::pipe(output_pipe) == -1) {
        throw std::runtime_error(std::string("pipe: ") + std::strerror(errno));
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

        std::vector<char*> child_arguments;
        child_arguments.reserve(arguments.size() + 2);
        child_arguments.push_back(const_cast<char*>(executable));
        for (const auto& argument : arguments) {
            child_arguments.push_back(const_cast<char*>(argument.c_str()));
        }
        child_arguments.push_back(nullptr);
        ::execv(executable, child_arguments.data());
        _exit(127);
    }

    ::close(output_pipe[1]);
    std::string output;
    char buffer[512];
    while (true) {
        const ssize_t count = ::read(output_pipe[0], buffer, sizeof(buffer));
        if (count > 0) {
            output.append(buffer, static_cast<std::size_t>(count));
            continue;
        }
        if (count == -1 && errno == EINTR) {
            continue;
        }
        break;
    }
    ::close(output_pipe[0]);

    int status = 0;
    while (::waitpid(child, &status, 0) == -1) {
        if (errno != EINTR) {
            throw std::runtime_error(std::string("waitpid: ") +
                                     std::strerror(errno));
        }
    }

    const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    return {exit_code, std::move(output)};
}

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void expect_contains(const std::string& output, const std::string& text,
                     const std::string& message) {
    expect(output.find(text) != std::string::npos, message);
}

int reserve_loopback_port(std::uint16_t& port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd == -1) {
        throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) == -1 ||
        ::listen(fd, 1) == -1) {
        const int error_number = errno;
        ::close(fd);
        throw std::runtime_error(std::string("reserve port: ") +
                                 std::strerror(error_number));
    }
    socklen_t length = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == -1) {
        const int error_number = errno;
        ::close(fd);
        throw std::runtime_error(std::string("getsockname: ") +
                                 std::strerror(error_number));
    }
    port = ntohs(address.sin_port);
    return fd;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: cli_tests <hp_http_server-path>\n";
        return 2;
    }

    try {
        const RunResult help = run_process(argv[1], {"--help"});
        expect(help.exit_code == 0, "--help must exit 0");
        expect_contains(help.output, "Usage:", "--help must print usage");
        expect_contains(help.output, "does not provide HTTP service",
                        "--help must preserve the non-HTTP boundary");
        expect_contains(help.output, "--port <0-65535>",
                        "--help must document the explicit port syntax");

        const RunResult default_run = run_process(argv[1], {});
        expect(default_run.exit_code != 0, "missing --port must exit non-zero");
        expect_contains(default_run.output, "expected exactly one --port",
                        "missing --port must be diagnosed");

        const RunResult unknown = run_process(argv[1], {"--unknown-option"});
        expect(unknown.exit_code != 0, "unknown option must exit non-zero");
        expect_contains(unknown.output, "Usage:",
                        "unknown option must print usage");

        const std::vector<std::vector<std::string>> invalid_cases = {
            {"--port"},
            {"--port", ""},
            {"--port", "invalid"},
            {"--port", "-1"},
            {"--port", "65536"},
            {"--port", "12x"},
            {"--port", "1", "--port", "2"},
            {"--help", "--port", "0"},
        };
        for (const auto& arguments : invalid_cases) {
            const RunResult invalid = run_process(argv[1], arguments);
            expect(invalid.exit_code != 0,
                   "each malformed or duplicate CLI must exit non-zero");
            expect_contains(invalid.output, "Usage:",
                            "invalid CLI must include concise usage");
        }

        std::uint16_t occupied_port = 0;
        const int reservation = reserve_loopback_port(occupied_port);
        const RunResult bind_failure =
            run_process(argv[1], {"--port", std::to_string(occupied_port)});
        ::close(reservation);
        expect(bind_failure.exit_code != 0,
               "occupied port startup must exit non-zero");
        expect_contains(bind_failure.output, "bind",
                        "occupied port failure must name bind");
        expect_contains(bind_failure.output, "Address already in use",
                        "occupied port failure must retain the system error");
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }

    if (failures != 0) {
        std::cerr << failures << " CLI test assertion(s) failed\n";
        return 1;
    }
    std::cout << "CLI tests passed\n";
    return 0;
}
