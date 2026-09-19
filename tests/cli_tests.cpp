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

RunResult RunProcess(const char* executable,
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
      throw std::runtime_error(std::string("waitpid: ") + std::strerror(errno));
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
      throw std::runtime_error(std::string("mkdtemp: ") + std::strerror(errno));
    }
    workspace_ = created;
    root_ = workspace_ / "root";
    file_ = workspace_ / "not-a-directory";
    std::filesystem::create_directory(root_);
    std::ofstream(file_) << "not a root";
  }

  ~Fixture() {
    std::error_code ignored;
    std::filesystem::remove_all(workspace_, ignored);
  }

  std::filesystem::path workspace_;
  std::filesystem::path root_;
  std::filesystem::path file_;
};

int failures = 0;

void Expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void ExpectContains(const std::string& output,
                    const std::string& text,
                    const std::string& message) {
  Expect(output.find(text) != std::string::npos, message);
}

int ReserveLoopbackPort(std::uint16_t& port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd == -1) {
    throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(fd,
             reinterpret_cast<const sockaddr*>(&address),
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
    Fixture fixture;
    const RunResult help = RunProcess(argv[1], {"--help"});
    Expect(help.exit_code == 0, "--help must exit 0");
    ExpectContains(help.output,
                   "V0.1 / S3 minimal HTTP static file server",
                   "--help must identify S3 HTTP");
    ExpectContains(help.output,
                   "--root <directory>",
                   "--help must document root");

    const std::vector<std::vector<std::string>> invalid_cases = {
        {},
        {"--unknown-option"},
        {"--port"},
        {"--root"},
        {"--port", "0"},
        {"--root", fixture.root_.string()},
        {"--port", ""},
        {"--port", "invalid", "--root", fixture.root_.string()},
        {"--port", "-1", "--root", fixture.root_.string()},
        {"--port", "65536", "--root", fixture.root_.string()},
        {"--port", "12x", "--root", fixture.root_.string()},
        {"--port", "1", "--port", "2", "--root", fixture.root_.string()},
        {"--port",
         "0",
         "--root",
         fixture.root_.string(),
         "--root",
         fixture.root_.string()},
        {"--help", "--port", "0", "--root", fixture.root_.string()},
    };
    for (const auto& arguments : invalid_cases) {
      const RunResult invalid = RunProcess(argv[1], arguments);
      Expect(invalid.exit_code != 0,
             "malformed, missing or duplicate CLI must fail");
      ExpectContains(invalid.output,
                     "Usage:",
                     "invalid CLI must include concise usage");
    }

    for (const auto& value : {"65", "-1", "+1", "2x", "", "x"}) {
      const auto result = RunProcess(argv[1],
                                     {"--port",
                                      "0",
                                      "--root",
                                      fixture.root_.string(),
                                      "--threads",
                                      value});
      Expect(result.exit_code == 2, "invalid threads exit2");
    }
    Expect(RunProcess(
               argv[1],
               {"--port", "0", "--root", fixture.root_.string(), "--threads"})
                   .exit_code == 2,
           "threads missing value exit2");
    Expect(RunProcess(argv[1],
                      {"--port",
                       "0",
                       "--root",
                       fixture.root_.string(),
                       "--threads",
                       "1",
                       "--threads",
                       "2"})
                   .exit_code == 2,
           "duplicate threads exit2");
    ExpectContains(help.output, "defaults to 2", "help default workers");
    const std::string missing =
        (fixture.workspace_ / "private-missing-root").string();
    Expect(RunProcess(argv[1],
                      {"--port", "0", "--root", missing, "--threads", "64"})
                   .exit_code == 1,
           "64 parses before resource failure without starting threads");
    for (const auto option : {"--idle-timeout-ms", "--keep-alive-timeout-ms"}) {
      for (const auto value :
           {"86400001", "-1", "+1", "", "1x", "184467440737095516160"}) {
        Expect(RunProcess(argv[1],
                          {"--port",
                           "0",
                           "--root",
                           fixture.root_.string(),
                           option,
                           value})
                       .exit_code == 2,
               "invalid timeout exits 2");
      }
      Expect(
          RunProcess(argv[1],
                     {"--port", "0", "--root", fixture.root_.string(), option})
                  .exit_code == 2,
          "missing timeout exits 2");
      Expect(RunProcess(argv[1],
                        {"--port",
                         "0",
                         "--root",
                         fixture.root_.string(),
                         option,
                         "0",
                         option,
                         "0"})
                     .exit_code == 2,
             "duplicate timeout exits 2");
      for (const auto value : {"0", "1", "86400000"}) {
        Expect(RunProcess(argv[1],
                          {"--port", "0", "--root", missing, option, value})
                       .exit_code == 1,
               "valid timeout reaches resource validation exit 1");
      }
    }
    ExpectContains(help.output,
                   "defaults to 30000",
                   "help ordinary timeout default");
    ExpectContains(help.output,
                   "defaults to 15000",
                   "help keep-alive timeout default");
    for (const auto value :
         {"60001", "-1", "+1", "", "x", "184467440737095516160"}) {
      Expect(RunProcess(argv[1],
                        {"--port",
                         "0",
                         "--root",
                         fixture.root_.string(),
                         "--shutdown-timeout-ms",
                         value})
                     .exit_code == 2,
             "invalid shutdown timeout exits 2");
    }
    Expect(RunProcess(argv[1],
                      {"--port",
                       "0",
                       "--root",
                       fixture.root_.string(),
                       "--shutdown-timeout-ms"})
                   .exit_code == 2,
           "missing shutdown timeout exits 2");
    Expect(RunProcess(argv[1],
                      {"--port",
                       "0",
                       "--root",
                       fixture.root_.string(),
                       "--shutdown-timeout-ms",
                       "1",
                       "--shutdown-timeout-ms",
                       "0"})
                   .exit_code == 2,
           "duplicate shutdown timeout exits 2");
    for (const auto value : {"0", "1", "60000"}) {
      Expect(RunProcess(argv[1],
                        {"--port",
                         "0",
                         "--root",
                         missing,
                         "--shutdown-timeout-ms",
                         value})
                     .exit_code == 1,
             "valid shutdown timeout reaches resource validation");
    }
    ExpectContains(help.output, "defaults to 5000", "help shutdown default");
    const RunResult missing_root =
        RunProcess(argv[1], {"--port", "0", "--root", missing});
    Expect(missing_root.exit_code != 0,
           "missing root must fail before serving");
    ExpectContains(missing_root.output,
                   "static root is unavailable",
                   "missing root must have a concise diagnosis");
    Expect(missing_root.output.find(missing) == std::string::npos,
           "root failure must not echo a private absolute path");

    const RunResult file_root =
        RunProcess(argv[1], {"--root", fixture.file_.string(), "--port", "0"});
    Expect(file_root.exit_code != 0, "non-directory root must fail");
    Expect(file_root.output.find(fixture.file_.string()) == std::string::npos,
           "non-directory failure must not echo its absolute path");

    std::uint16_t occupied_port = 0;
    const int reservation = ReserveLoopbackPort(occupied_port);
    const RunResult bind_failure = RunProcess(argv[1],
                                              {"--root",
                                               fixture.root_.string(),
                                               "--port",
                                               std::to_string(occupied_port)});
    ::close(reservation);
    Expect(bind_failure.exit_code != 0,
           "occupied port startup must exit non-zero");
    ExpectContains(bind_failure.output,
                   "bind",
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
