#define main hp_server_entry
#include "../app/main.cpp"
#undef main

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>

#include "async_logger_faults.h"
#include "async_logger_test_support.h"

namespace {
using namespace logger_test;

class ReadyOutput : public std::streambuf {
 public:
  std::string await_ready() {
    std::unique_lock lock(mutex_);
    if (!cv_.wait_for(lock, 3s,
                      [this] { return text_.find('\n') != std::string::npos; }))
      return {};
    return text_;
  }

 protected:
  std::streamsize xsputn(const char* data, std::streamsize size) override {
    const std::lock_guard lock(mutex_);
    text_.append(data, size);
    cv_.notify_all();
    return size;
  }

  int overflow(int ch) override {
    const char value = ch;
    xsputn(&value, 1);
    return ch;
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::string text_;
};

int connect_to(unsigned port) {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  timeval timeout{2, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

std::size_t tasks() {
  return std::distance(std::filesystem::directory_iterator("/proc/self/task"),
                       std::filesystem::directory_iterator{});
}

void production(unsigned threads, const std::string& root) {
  Gate gate;
  gate.blocked = true;
  ReadyOutput ready;
  auto* old_log = std::clog.rdbuf(&gate);
  auto* old_out = std::cout.rdbuf(&ready);
  // Test helper must inherit the same mask as the real entry/consumer/workers.
  ShutdownSignalMask helper_mask;
  const auto owner = std::this_thread::get_id();
  bool request_ok = false, control_ok = false, workers_joined = false;
  std::string error;
  std::promise<void> helper_started;
  auto started = helper_started.get_future();
  std::thread client([&] {
    const auto joined_task_count = tasks() + 1;
    helper_started.set_value();
    try {
      require(gate.await_entry(), "real consumer entered sink");
      const auto line = ready.await_ready();
      const auto position = line.find("listening on port ");
      require(position != std::string::npos, "stdout readiness preserved");
      const unsigned port = std::stoul(line.substr(position + 18));
      int fd = connect_to(port);
      require(fd >= 0, "healthy connect while sink blocked");
      const std::string request =
          "GET /index.html HTTP/1.1\r\nHost: localhost\r\nConnection: "
          "close\r\n\r\n";
      const auto sent =
          ::send(fd, request.data(), request.size(), MSG_NOSIGNAL);
      std::string response;
      char buffer[4096];
      ssize_t count;
      while ((count = ::recv(fd, buffer, sizeof(buffer), 0)) > 0)
        response.append(buffer, count);
      ::close(fd);
      require(sent == static_cast<ssize_t>(request.size()) && count == 0 &&
                  response.find("HTTP/1.1 200 OK\r\n") == 0 &&
                  response.ends_with("logger healthy\n"),
              "complete HTTP and EOF before sink release");
      request_ok = true;
      ::kill(::getpid(), SIGTERM);
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      while (std::chrono::steady_clock::now() < deadline) {
        fd = connect_to(port);
        if (fd < 0) {
          control_ok = true;
          break;
        }
        ::close(fd);
        std::this_thread::yield();
      }
      while (std::chrono::steady_clock::now() < deadline) {
        if (tasks() == joined_task_count) {
          workers_joined = true;
          break;
        }
        std::this_thread::yield();
      }
    } catch (const std::exception& e) {
      error = e.what();
      ::kill(::getpid(), SIGTERM);
    }
    gate.release();
  });
  started.get();
  std::string worker_count = std::to_string(threads);
  const char* args[] = {"hp_http_server",
                        "--port",
                        "0",
                        "--root",
                        root.c_str(),
                        "--threads",
                        worker_count.c_str(),
                        "--shutdown-timeout-ms",
                        "100"};
  const int result = hp_server_entry(9, const_cast<char**>(args));
  client.join();
  std::clog.rdbuf(old_log);
  std::cout.rdbuf(old_out);
  require(error.empty(), error.c_str());
  require(result == 0 && request_ok && control_ok && workers_joined,
          "production HTTP control workers before logger join");
  require(gate.writers().size() == 1 && !gate.writers().contains(owner),
          "real production all sink calls unique non-owner consumer");
  require(gate.text().find("[INFO] Shutdown signal observed: 15.\n") !=
              std::string::npos,
          "owner shutdown diagnostic queued and drained");
  std::cout << "production threads=" << threads << " owner=" << owner
            << " consumer=" << *gate.writers().begin()
            << " HTTP_EOF=1 control=1 workers_joined_before_release=1 PASS\n";
}
}  // namespace

int main() {
  try {
    sigset_t before{}, after{};
    ::pthread_sigmask(SIG_SETMASK, nullptr, &before);
    const auto root =
        std::filesystem::path(std::getenv("HP_S3_TEST_TMP_ROOT")) /
        ("logger-production-" + std::to_string(::getpid()));
    std::filesystem::create_directories(root);
    std::ofstream(root / "index.html") << "logger healthy\n";
    for (unsigned threads : {0, 1, 2}) production(threads, root.string());
    ::pthread_sigmask(SIG_SETMASK, nullptr, &after);
    require(::sigismember(&before, SIGTERM) == ::sigismember(&after, SIGTERM) &&
                ::sigismember(&before, SIGINT) == ::sigismember(&after, SIGINT),
            "entry restored prior thread signal mask");
    std::filesystem::remove_all(root);
    Gate fatal;
    auto* old = std::clog.rdbuf(&fatal);
    const char* args[] = {"server", "--port", "0", "--root", "/dev/null"};
    const int result = hp_server_entry(5, const_cast<char**>(args));
    std::clog.rdbuf(old);
    require(result == 1 && fatal.text().starts_with("[ERROR] ") &&
                fatal.writers().size() == 1 &&
                !fatal.writers().contains(std::this_thread::get_id()),
            "real fatal catch consumed before logger destruction");
    std::cout << "production fatal and prior signal mask restoration PASS\n";
    for (int mode = 0; mode < 2; ++mode) {
      const auto initial_tasks = tasks();
      Gate diagnostic;
      std::stringbuf stdout_capture;
      old = std::clog.rdbuf(&diagnostic);
      auto* old_stdout = std::cout.rdbuf(&stdout_capture);
      // Start in a fresh process-global session state for the failure
      // diagnostic: after a prior session, compatibility routing intentionally
      // remains stopped.
      allocation_size = hp::base::AsyncLoggerTestAccess::allocation_bytes();
      fail_allocation = mode == 0;
      fail_thread = mode == 1;
      const int failed = hp_server_entry(5, const_cast<char**>(args));
      fail_allocation = false;
      fail_thread = false;
      std::clog.rdbuf(old);
      std::cout.rdbuf(old_stdout);
      ::pthread_sigmask(SIG_SETMASK, nullptr, &after);
      require(
          failed == 1 && stdout_capture.str().empty() &&
              tasks() == initial_tasks &&
              ::sigismember(&before, SIGTERM) == ::sigismember(&after, SIGTERM),
          "startup failure before listen with thread cleanup and mask restore");
    }
    require(allocation_hits == 1 && thread_hits == 1,
            "production startup exact hits");
    std::cout << "production startup allocation/thread hits=1/1 no-listen "
                 "cleanup PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "production logger assertion: " << error.what() << '\n';
    return 1;
  }
}
