#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
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

class Fixture {
   public:
    Fixture() {
        const char* configured = std::getenv("HP_S3_TEST_TMP_ROOT");
        const std::filesystem::path base =
            configured == nullptr
                ? std::filesystem::path(".cache/olympus-v0.1-s3/tests")
                : std::filesystem::path(configured);
        std::filesystem::create_directories(base);
        std::string pattern = (base / "cli-XXXXXX").string();
        std::vector<char> storage(pattern.begin(), pattern.end());
        storage.push_back('\0');
        char* created = ::mkdtemp(storage.data());
        if (created == nullptr) {
            throw std::runtime_error(std::string("mkdtemp: ") +
                                     std::strerror(errno));
        }
        workspace = created;
        root = workspace / "root";
        file = workspace / "not-a-directory";
        std::filesystem::create_directory(root);
        std::ofstream(file) << "not a root";
    }

    ~Fixture() {
        std::error_code ignored;
        std::filesystem::remove_all(workspace, ignored);
    }

    std::filesystem::path workspace;
    std::filesystem::path root;
    std::filesystem::path file;
};

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
        throw std::runtime_error(std::string("socket: ") +
                                 std::strerror(errno));
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
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) ==
        -1) {
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
        Fixture fixture;
        const RunResult help = run_process(argv[1], {"--help"});
        expect(help.exit_code == 0, "--help must exit 0");
        expect_contains(help.output,
                        "V0.1 / S3 minimal HTTP static file server",
                        "--help must identify S3 HTTP");
        expect_contains(help.output, "--root <directory>",
                        "--help must document root");

        const std::vector<std::vector<std::string>> invalid_cases = {
            {},
            {"--unknown-option"},
            {"--port"},
            {"--root"},
            {"--port", "0"},
            {"--root", fixture.root.string()},
            {"--port", ""},
            {"--port", "invalid", "--root", fixture.root.string()},
            {"--port", "-1", "--root", fixture.root.string()},
            {"--port", "65536", "--root", fixture.root.string()},
            {"--port", "12x", "--root", fixture.root.string()},
            {"--port", "1", "--port", "2", "--root", fixture.root.string()},
            {"--port", "0", "--root", fixture.root.string(), "--root",
             fixture.root.string()},
            {"--help", "--port", "0", "--root", fixture.root.string()},
        };
        for (const auto& arguments : invalid_cases) {
            const RunResult invalid = run_process(argv[1], arguments);
            expect(invalid.exit_code != 0,
                   "malformed, missing or duplicate CLI must fail");
            expect_contains(invalid.output,
                            "Usage:", "invalid CLI must include concise usage");
        }

        const std::string missing =
            (fixture.workspace / "private-missing-root").string();
        const RunResult missing_root =
            run_process(argv[1], {"--port", "0", "--root", missing});
        expect(missing_root.exit_code != 0,
               "missing root must fail before serving");
        expect_contains(missing_root.output, "static root is unavailable",
                        "missing root must have a concise diagnosis");
        expect(missing_root.output.find(missing) == std::string::npos,
               "root failure must not echo a private absolute path");

        const RunResult file_root = run_process(
            argv[1], {"--root", fixture.file.string(), "--port", "0"});
        expect(file_root.exit_code != 0, "non-directory root must fail");
        expect(
            file_root.output.find(fixture.file.string()) == std::string::npos,
            "non-directory failure must not echo its absolute path");

        std::uint16_t occupied_port = 0;
        const int reservation = reserve_loopback_port(occupied_port);
        const RunResult bind_failure =
            run_process(argv[1], {"--root", fixture.root.string(), "--port",
                                  std::to_string(occupied_port)});
        ::close(reservation);
        expect(bind_failure.exit_code != 0,
               "occupied port startup must exit non-zero");
        expect_contains(bind_failure.output, "bind",
                        "occupied port failure must name bind");
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }

    if (failures != 0) {
        std::cerr << failures << " CLI test assertion(s) failed\n";
        return 1;
    }
    std::cout << "CLI tests passed; invalid_cases=14 root_preflight_cases=2\n";
    return 0;
}
