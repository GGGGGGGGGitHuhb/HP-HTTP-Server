#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
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
                        "--help must state that HTTP service is unavailable");

        const RunResult default_run = run_process(argv[1], {});
        expect(default_run.exit_code == 0, "default run must exit 0");
        expect_contains(default_run.output, "V0.1 / S1 skeleton",
                        "default run must identify the S1 skeleton");
        expect_contains(default_run.output, "does not provide HTTP service",
                        "default run must state that HTTP service is unavailable");

        const RunResult unknown = run_process(argv[1], {"--unknown-option"});
        expect(unknown.exit_code != 0, "unknown option must exit non-zero");
        expect_contains(unknown.output, "unknown option",
                        "unknown option must print an error");
        expect_contains(unknown.output, "Usage:",
                        "unknown option must print usage");
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
