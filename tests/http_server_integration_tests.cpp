#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kRequestLimit = 16U * 1024U;
constexpr std::size_t kFileLimit = 8U * 1024U * 1024U;
constexpr std::string_view kSecret = "S3-INTEGRATION-SIBLING-SECRET";
int failures = 0;
int status_200_hits = 0;
int status_400_hits = 0;
int status_403_hits = 0;
int status_404_hits = 0;
int status_405_hits = 0;
int segmented_need_more_hits = 0;
int write_eagain_hits = 0;
int accept_drain_successes = 0;
int isolation_successes = 0;
std::size_t server_fd_baseline = 0;
std::size_t server_fd_after = 0;

void Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
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
    std::string pattern = (base / "integration-XXXXXX").string();
    std::vector<char> storage(pattern.begin(), pattern.end());
    storage.push_back('\0');
    char* created = ::mkdtemp(storage.data());
    if (created == nullptr) {
      throw std::runtime_error(std::string("mkdtemp: ") + std::strerror(errno));
    }
    workspace_ = created;
    root_ = workspace_ / "root";
    sibling_ = workspace_ / "sibling-secret.txt";
    std::filesystem::create_directories(root_ / "assets");
    WriteText(root_ / "index.html", "<h1>integration index</h1>\n");
    WriteText(root_ / "note.txt", "hello from S3\n");
    WriteText(root_ / "assets" / "unknown.blob", "unknown mime\n");
    WriteText(sibling_, kSecret);
    std::filesystem::create_symlink(sibling_, root_ / "escape.txt");

    binary_ = {std::byte{0x00},
               std::byte{0x01},
               std::byte{0x7f},
               std::byte{0xff},
               std::byte{0x41},
               std::byte{0x00}};
    WriteBytes(root_ / "assets" / "binary.png", binary_);

    large_.resize(kFileLimit);
    for (std::size_t index = 0; index < large_.size(); ++index) {
      large_[index] = static_cast<std::byte>((index * 31U + 7U) & 0xffU);
    }
    WriteBytes(root_ / "large.bin", large_);

    const int fd = ::open((root_ / "oversized.bin").c_str(),
                          O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                          0600);
    if (fd == -1 || ::ftruncate(fd, static_cast<off_t>(kFileLimit + 1)) == -1) {
      const int error_number = errno;
      if (fd >= 0) ::close(fd);
      throw std::runtime_error(std::string("oversized fixture: ") +
                               std::strerror(error_number));
    }
    ::close(fd);
  }

  ~Fixture() {
    std::error_code ignored;
    std::filesystem::remove_all(workspace_, ignored);
  }

  static void WriteText(const std::filesystem::path& path,
                        std::string_view data) {
    std::ofstream output(path, std::ios::binary);
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!output) throw std::runtime_error("fixture text write failed");
  }

  static void WriteBytes(const std::filesystem::path& path,
                         const std::vector<std::byte>& data) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(data.data()),
                 static_cast<std::streamsize>(data.size()));
    if (!output) throw std::runtime_error("fixture binary write failed");
  }

  std::filesystem::path workspace_;
  std::filesystem::path root_;
  std::filesystem::path sibling_;
  std::vector<std::byte> binary_;
  std::vector<std::byte> large_;
};

class ServerProcess {
 public:
  ServerProcess(pid_t pid,
                int output_fd,
                std::uint16_t port,
                std::string output)
      : pid_(pid),
        output_fd_(output_fd),
        port_(port),
        output_(std::move(output)) {}

  ~ServerProcess() { Stop(); }

  ServerProcess(const ServerProcess&) = delete;
  ServerProcess& operator=(const ServerProcess&) = delete;

  ServerProcess(ServerProcess&& other) noexcept
      : pid_(std::exchange(other.pid_, -1)),
        output_fd_(std::exchange(other.output_fd_, -1)),
        port_(other.port_),
        output_(std::move(other.output_)) {}

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

  [[nodiscard]] const std::string& output() const noexcept { return output_; }

  [[nodiscard]] bool Running() const noexcept {
    return pid_ > 0 && ::kill(pid_, 0) == 0;
  }

  [[nodiscard]] std::size_t fd_count() const {
    std::size_t count = 0;
    const std::filesystem::path fd_root =
        "/proc/" + std::to_string(pid_) + "/fd";
    for (const auto& ignored : std::filesystem::directory_iterator(fd_root)) {
      (void)ignored;
      ++count;
    }
    return count;
  }

  [[nodiscard]] std::size_t thread_count() const {
    return static_cast<std::size_t>(std::distance(
        std::filesystem::directory_iterator("/proc/" + std::to_string(pid_) +
                                            "/task"),
        {}));
  }

  void DrainOutput(std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
      pollfd descriptor{output_fd_, POLLIN, 0};
      const int result = ::poll(&descriptor, 1, 20);
      if (result == -1 && errno == EINTR) continue;
      if (result <= 0 || (descriptor.revents & POLLIN) == 0) continue;
      char buffer[1024];
      while (true) {
        const ssize_t count = ::read(output_fd_, buffer, sizeof(buffer));
        if (count > 0) {
          output_.append(buffer, static_cast<std::size_t>(count));
          continue;
        }
        if (count == -1 && errno == EINTR) continue;
        break;
      }
    }
  }

  void Stop() noexcept {
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
  std::string output_;
};

ServerProcess StartServer(const char* executable,
                          const std::filesystem::path& root,
                          bool root_first = false) {
  int output_pipe[2] = {-1, -1};
  if (::pipe2(output_pipe, O_CLOEXEC) == -1) {
    throw std::runtime_error(std::string("pipe2: ") + std::strerror(errno));
  }
  const pid_t child = ::fork();
  if (child == -1) {
    throw std::runtime_error(std::string("fork: ") + std::strerror(errno));
  }
  if (child == 0) {
    ::close(output_pipe[0]);
    if (::dup2(output_pipe[1], STDOUT_FILENO) == -1 ||
        ::dup2(output_pipe[1], STDERR_FILENO) == -1) {
      _exit(126);
    }
    ::close(output_pipe[1]);
    const std::string root_text = root.string();
    std::vector<std::string> arguments{executable};
    if (root_first) {
      arguments.insert(arguments.end(), {"--root", root_text, "--port", "0"});
    } else {
      arguments.insert(arguments.end(), {"--port", "0", "--root", root_text});
    }
    if (const char* threads = std::getenv("HP_HTTP_TEST_THREADS"))
      arguments.insert(arguments.end(), {"--threads", threads});
    std::vector<char*> pointers;
    for (auto& argument : arguments) pointers.push_back(argument.data());
    pointers.push_back(nullptr);
    ::execv(executable, pointers.data());
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
    throw std::runtime_error(std::string("fcntl: ") +
                             std::strerror(error_number));
  }

  std::string output;
  constexpr std::string_view kMarker =
      "V0.1 / S3 minimal HTTP static file server listening on port ";
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd descriptor{output_pipe[0], POLLIN, 0};
    const int result = ::poll(&descriptor, 1, 100);
    if (result == -1 && errno == EINTR) continue;
    if (result > 0 && (descriptor.revents & POLLIN) != 0) {
      char buffer[1024];
      while (true) {
        const ssize_t count = ::read(output_pipe[0], buffer, sizeof(buffer));
        if (count > 0) {
          output.append(buffer, static_cast<std::size_t>(count));
          continue;
        }
        if (count == -1 && errno == EINTR) continue;
        break;
      }
    }
    const std::size_t marker_position = output.find(kMarker);
    if (marker_position != std::string::npos) {
      const std::size_t start = marker_position + kMarker.size();
      const std::size_t end = output.find_first_not_of("0123456789", start);
      const unsigned long parsed =
          std::stoul(output.substr(start, end - start));
      if (parsed > 0 && parsed <= 65535) {
        return ServerProcess(child,
                             output_pipe[0],
                             static_cast<std::uint16_t>(parsed),
                             output);
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
  throw std::runtime_error("timed out waiting for startup: " + output);
}

int ConnectClient(std::uint16_t port, int receive_buffer = 0) {
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd == -1) {
    throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
  }
  if (receive_buffer > 0) {
    (void)::setsockopt(fd,
                       SOL_SOCKET,
                       SO_RCVBUF,
                       &receive_buffer,
                       sizeof(receive_buffer));
  }
  timeval timeout{8, 0};
  (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == -1) {
    const int error_number = errno;
    ::close(fd);
    throw std::runtime_error(std::string("connect: ") +
                             std::strerror(error_number));
  }
  return fd;
}

void SendAll(int fd, std::string_view data) {
  std::size_t offset = 0;
  while (offset < data.size()) {
    const ssize_t count =
        ::send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count == -1 && errno == EINTR) continue;
    throw std::runtime_error(std::string("send: ") + std::strerror(errno));
  }
}

std::vector<std::byte> ReceiveToEof(int fd) {
  std::vector<std::byte> received;
  std::vector<std::byte> buffer(64 * 1024);
  while (true) {
    const ssize_t count = ::recv(fd, buffer.data(), buffer.size(), 0);
    if (count > 0) {
      received.insert(received.end(), buffer.begin(), buffer.begin() + count);
      continue;
    }
    if (count == 0) break;
    if (errno == EINTR) continue;
    throw std::runtime_error(std::string("recv: ") + std::strerror(errno));
  }
  return received;
}

std::vector<std::byte> Transact(std::uint16_t port,
                                std::string_view request,
                                int receive_buffer = 0,
                                std::chrono::milliseconds pause_before_read =
                                    std::chrono::milliseconds(0)) {
  const int client = ConnectClient(port, receive_buffer);
  SendAll(client, request);
  (void)::shutdown(client, SHUT_WR);
  if (pause_before_read.count() > 0) {
    ::usleep(static_cast<useconds_t>(pause_before_read.count() * 1000));
  }
  auto response = ReceiveToEof(client);
  ::close(client);
  return response;
}

struct Response {
  int status;
  std::string header;
  std::vector<std::byte> body;
};

Response ParseResponse(const std::vector<std::byte>& bytes) {
  const std::string_view raw(reinterpret_cast<const char*>(bytes.data()),
                             bytes.size());
  const std::size_t boundary = raw.find("\r\n\r\n");
  if (boundary == std::string_view::npos || raw.size() < 12) {
    throw std::runtime_error("malformed HTTP response");
  }
  const int status = std::stoi(std::string(raw.substr(9, 3)));
  if (status == 200) ++status_200_hits;
  if (status == 400) ++status_400_hits;
  if (status == 403) ++status_403_hits;
  if (status == 404) ++status_404_hits;
  if (status == 405) ++status_405_hits;
  std::string header(raw.substr(0, boundary + 4));
  const std::string length_name = "Content-Length: ";
  const std::size_t length_start = header.find(length_name);
  Expect(length_start != std::string::npos,
         "every response must include Content-Length");
  if (length_start != std::string::npos) {
    const std::size_t value_start = length_start + length_name.size();
    const std::size_t value_end = header.find("\r\n", value_start);
    const std::size_t declared =
        std::stoull(header.substr(value_start, value_end - value_start));
    Expect(declared == bytes.size() - boundary - 4,
           "Content-Length must equal body bytes");
  }
  Expect(header.find("Connection: close\r\n") != std::string::npos,
         "every response must declare Connection: close");
  return {status,
          std::move(header),
          std::vector<std::byte>(bytes.begin() + boundary + 4, bytes.end())};
}

std::string BodyText(const Response& response) {
  return {reinterpret_cast<const char*>(response.body.data()),
          response.body.size()};
}

Response Request(std::uint16_t port,
                 std::string_view target,
                 std::string_view method = "GET",
                 std::string_view extra_headers = {}) {
  std::string raw = std::string(method) + " " + std::string(target) +
                    " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n" +
                    std::string(extra_headers) + "\r\n";
  return ParseResponse(Transact(port, raw));
}

void TestSuccess(std::uint16_t port, const Fixture& fixture) {
  const Response index = Request(port, "/");
  Expect(
      index.status == 200 && BodyText(index) == "<h1>integration index</h1>\n",
      "GET / must return exact index bytes");
  Expect(index.header.find("Content-Type: text/html; charset=utf-8\r\n") !=
             std::string::npos,
         "index must use HTML MIME");

  const Response text = Request(port, "/note.txt?cache=no");
  Expect(text.status == 200 && BodyText(text) == "hello from S3\n",
         "text query request must return exact file");

  const Response binary = Request(port, "/assets/binary.png");
  Expect(binary.status == 200 && binary.body == fixture.binary_,
         "binary file body must preserve NUL and all bytes");

  const Response unknown = Request(port, "/assets/unknown.blob");
  Expect(
      unknown.status == 200 &&
          unknown.header.find("Content-Type: application/octet-stream\r\n") !=
              std::string::npos,
      "unknown extension must use octet-stream");
}

void TestErrorsAndPaths(std::uint16_t port) {
  Expect(Request(port, "/missing.txt").status == 404,
         "missing file must return 404");
  const Response post = Request(port, "/", "POST");
  Expect(post.status == 405 &&
             post.header.find("Allow: GET\r\n") != std::string::npos,
         "POST must return 405 with Allow");

  const std::vector<std::string> bad_requests = {
      "GET / HTTP/1.1\r\nUser-Agent: x\r\n\r\n",
      "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n",
      "GET / HTTP/1.1\nHost: x\n\n",
  };
  for (const auto& raw : bad_requests) {
    Expect(ParseResponse(Transact(port, raw)).status == 400,
           "bad request must return 400");
  }

  const std::vector<std::pair<std::string, int>> paths = {
      {"/../sibling-secret.txt", 403},
      {"/./index.html", 403},
      {"//index.html", 403},
      {"/assets\\unknown.blob", 403},
      {"/%2e%2e/sibling-secret.txt", 400},
      {"/escape.txt", 403},
      {"/oversized.bin", 403},
  };
  for (const auto& [target, status] : paths) {
    const Response response = Request(port, target);
    Expect(response.status == status,
           "path rejection must use its designed status");
    Expect(BodyText(response).find(kSecret) == std::string::npos,
           "rejected path must never return sibling secret");
  }
}

void TestAcceptDrain(std::uint16_t port) {
  std::vector<int> clients;
  for (int index = 0; index < 8; ++index) {
    clients.push_back(ConnectClient(port));
  }
  const std::string raw =
      "GET /note.txt HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  for (int client : clients) {
    SendAll(client, raw);
    (void)::shutdown(client, SHUT_WR);
  }
  for (int client : clients) {
    const Response response = ParseResponse(ReceiveToEof(client));
    if (response.status == 200 && BodyText(response) == "hello from S3\n") {
      ++accept_drain_successes;
    }
    ::close(client);
  }
  Expect(accept_drain_successes == 8,
         "eight queued connections must each receive their own response");
}

void TestSegmentedLimitsAndHalfClose(std::uint16_t port) {
  const int segmented = ConnectClient(port);
  SendAll(segmented, "GET /note.txt HTTP/1.1\r\nHost:");
  pollfd descriptor{segmented, POLLIN, 0};
  const int early = ::poll(&descriptor, 1, 100);
  Expect(early == 0, "first request fragment must not produce a response");
  if (early == 0) ++segmented_need_more_hits;
  SendAll(segmented, " localhost\r\nConnection: close\r\n\r\n");
  (void)::shutdown(segmented, SHUT_WR);
  const Response completed = ParseResponse(ReceiveToEof(segmented));
  Expect(completed.status == 200, "second fragment must complete one response");
  ::close(segmented);

  std::string request_at_limit =
      "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\nX-Limit: ";
  request_at_limit.append(kRequestLimit - request_at_limit.size(), 'a');
  Expect(request_at_limit.size() == kRequestLimit,
         "limit fixture must be exactly 16 KiB");
  Expect(ParseResponse(Transact(port, request_at_limit)).status == 400,
         "16 KiB incomplete headers must return 400");

  const Response half_closed = ParseResponse(
      Transact(port,
               "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"));
  Expect(half_closed.status == 400,
         "incomplete request followed by write half-close must return 400");

  const std::string first =
      "GET /note.txt HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  const std::string second =
      "GET /missing.txt HTTP/1.1\r\nHost: localhost\r\nConnection: "
      "close\r\n\r\n";
  const auto pipelined_bytes = Transact(port, first + second);
  const Response pipelined = ParseResponse(pipelined_bytes);
  const std::string raw(reinterpret_cast<const char*>(pipelined_bytes.data()),
                        pipelined_bytes.size());
  Expect(
      pipelined.status == 200 &&
          raw.find("HTTP/1.1", raw.find("HTTP/1.1") + 1) == std::string::npos,
      "pipelined bytes must produce exactly one response");
}

void TestShortWriteAndIsolation(ServerProcess& server, const Fixture& fixture) {
  const std::string raw =
      "GET /large.bin HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  const Response large = ParseResponse(
      Transact(server.port(), raw, 4096, std::chrono::milliseconds(300)));
  Expect(large.status == 200 && large.body == fixture.large_,
         "paused large response must drain without loss after EAGAIN");
  server.DrainOutput(std::chrono::milliseconds(300));
  if (server.output().find("S3 evidence: connection write reached EAGAIN") !=
      std::string::npos) {
    ++write_eagain_hits;
  }
  Expect(write_eagain_hits > 0,
         "paused client must dynamically reach production write EAGAIN");

  const int reset_client = ConnectClient(server.port());
  linger reset_linger{1, 0};
  (void)::setsockopt(reset_client,
                     SOL_SOCKET,
                     SO_LINGER,
                     &reset_linger,
                     sizeof(reset_linger));
  SendAll(
      reset_client,
      "GET /large.bin HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n");
  ::close(reset_client);

  for (int attempt = 0; attempt < 20; ++attempt) {
    if (Request(server.port(), "/note.txt").status == 200) {
      ++isolation_successes;
    }
  }
  Expect(isolation_successes == 20 && server.Running(),
         "reset connection must not affect 20 later requests");
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::cerr << "Usage: http_server_integration_tests "
                 "<hp_http_server-path>\n";
    return 2;
  }

  try {
    Fixture fixture;
    ServerProcess reverse = StartServer(argv[1], fixture.root_, true);
    Expect(reverse.port() != 0, "root-first CLI order and port 0 must start");
    reverse.Stop();

    ServerProcess server = StartServer(argv[1], fixture.root_);
    Expect(server.port() != 0, "port 0 must report a real port");
    Expect(server.output().find("V0.1 / S3 minimal HTTP static file server") !=
               std::string::npos,
           "startup must identify S3 HTTP");
    server_fd_baseline = server.fd_count();
    TestSuccess(server.port(), fixture);
    TestErrorsAndPaths(server.port());
    TestAcceptDrain(server.port());
    TestSegmentedLimitsAndHalfClose(server.port());
    TestShortWriteAndIsolation(server, fixture);
    server_fd_after = server.fd_count();
    Expect(server_fd_after == server_fd_baseline,
           "server fd count must return to its startup baseline");
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }

  if (failures != 0) {
    std::cerr << failures << " HTTP integration assertion(s) failed\n";
    return 1;
  }
  std::cout << "HTTP integration tests passed; status_hits={200:"
            << status_200_hits << ",400:" << status_400_hits
            << ",403:" << status_403_hits << ",404:" << status_404_hits
            << ",405:" << status_405_hits << "}"
            << " segmented_need_more_hits=" << segmented_need_more_hits
            << " write_eagain_hits=" << write_eagain_hits
            << " path_secret_leaks=0 accept_drain_successes="
            << accept_drain_successes
            << " isolation_successes=" << isolation_successes
            << " server_fd_baseline=" << server_fd_baseline
            << " server_fd_after=" << server_fd_after << '\n';
  return 0;
}
