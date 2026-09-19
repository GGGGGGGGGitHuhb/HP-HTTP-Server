#include <pthread.h>

#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "base/async_logger.h"
#include "base/logger.h"
#include "http/http_request.h"
#include "http/http_response.h"
#include "http/static_file_service.h"
#include "http_connection_handler.h"
#include "net/tcp_server.h"
#include "signal_watcher.h"

namespace {

// The logger starts before SignalWatcher, so every background thread must
// already inherit the shutdown mask. Restore only after logger join.
class ShutdownSignalMask {
 public:
  ShutdownSignalMask() {
    sigset_t signals;
    ::sigemptyset(&signals);
    ::sigaddset(&signals, SIGINT);
    ::sigaddset(&signals, SIGTERM);
    const int result = ::pthread_sigmask(SIG_BLOCK, &signals, &previous_);
    if (result)
      throw std::system_error(result,
                              std::generic_category(),
                              "block logger shutdown signals");
  }

  ~ShutdownSignalMask() { ::pthread_sigmask(SIG_SETMASK, &previous_, nullptr); }

 private:
  sigset_t previous_{};
};

void PrintUsage(std::ostream& output) {
  output << "Usage: hp_http_server --port <0-65535> --root <directory> "
            "[--threads <0-64>]\n"
         << "       hp_http_server --root <directory> --port <0-65535>\n"
         << "       hp_http_server --help\n"
         << "V0.1 / S3 minimal HTTP static file server; restricted "
            "GET/keep-alive.\n"
         << "--threads defaults to 2 workers; 0 selects a single Reactor.\n"
         << "--idle-timeout-ms <0-86400000> defaults to 30000; "
            "--keep-alive-timeout-ms <0-86400000> defaults to 15000.\n"
         << "--shutdown-timeout-ms <0-60000> defaults to 5000; 0 closes "
            "immediately.\n"
         << "SIGINT/SIGTERM drain current output; another observed signal "
            "forces close.\n"
         << "Idle/keep-alive: 0 disables; expiry closes silently and may "
            "truncate a response.\n";
}

[[nodiscard]] std::uint16_t ParsePort(std::string_view text) {
  unsigned int value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value, 10);
  if (text.empty() || error != std::errc{} ||
      end != text.data() + text.size() || value > 65535U) {
    throw std::invalid_argument("port must be a decimal value in 0-65535");
  }
  return static_cast<std::uint16_t>(value);
}

[[nodiscard]] std::size_t ParseWorkerCount(std::string_view value) {
  unsigned int count = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), count, 10);
  if (value.empty() || error != std::errc{} ||
      end != value.data() + value.size() || count > 64)
    throw std::invalid_argument("threads must be decimal in 0-64");
  return count;
}

[[nodiscard]] std::chrono::milliseconds ParseShutdownTimeout(
    std::string_view value) {
  unsigned int milliseconds = 0;
  const auto [end, error] = std::from_chars(value.data(),
                                            value.data() + value.size(),
                                            milliseconds,
                                            10);
  if (value.empty() || error != std::errc{} ||
      end != value.data() + value.size() || milliseconds > 60000)
    throw std::invalid_argument(
        "shutdown timeout must be decimal in 0-60000ms");
  return std::chrono::milliseconds(milliseconds);
}

[[nodiscard]] std::chrono::milliseconds ParseConnectionTimeout(
    std::string_view value) {
  unsigned int value_ms = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), value_ms, 10);
  if (value.empty() || error != std::errc{} ||
      end != value.data() + value.size() || value_ms > 86400000)
    throw std::invalid_argument("timeout must be decimal in 0-86400000ms");
  return std::chrono::milliseconds(value_ms);
}

struct ServerOptions {
  std::uint16_t port{0};
  std::string root;
  std::size_t threads{2};
  std::chrono::milliseconds shutdown_timeout{5000};
  hp::net::ConnectionTimeouts timeouts{std::chrono::milliseconds(30000),
                                       std::chrono::milliseconds(15000)};
};

[[nodiscard]] ServerOptions ParseServerOptions(int argc, char* argv[]) {
  bool has_port = false;
  bool has_root = false;
  bool has_threads = false;
  bool has_idle_timeout = false, has_keep_alive_timeout = false,
       has_shutdown_timeout = false;
  ServerOptions options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view option = argv[index];
    if (option == "--port" || option == "--root" || option == "--threads" ||
        option == "--idle-timeout-ms" || option == "--keep-alive-timeout-ms" ||
        option == "--shutdown-timeout-ms") {
      if (index + 1 >= argc) {
        throw std::invalid_argument("option value is missing");
      }
      const std::string_view value = argv[++index];
      if (option == "--port") {
        if (has_port) {
          throw std::invalid_argument("--port appears more than once");
        }
        options.port = ParsePort(value);
        has_port = true;
      } else if (option == "--threads") {
        if (has_threads)
          throw std::invalid_argument("--threads appears more than once");
        options.threads = ParseWorkerCount(value);
        has_threads = true;
      } else if (option == "--shutdown-timeout-ms") {
        if (has_shutdown_timeout)
          throw std::invalid_argument(
              "shutdown timeout appears more than once");
        options.shutdown_timeout = ParseShutdownTimeout(value);
        has_shutdown_timeout = true;
      } else if (option == "--idle-timeout-ms" ||
                 option == "--keep-alive-timeout-ms") {
        bool& seen = option == "--idle-timeout-ms" ? has_idle_timeout
                                                   : has_keep_alive_timeout;
        if (seen) throw std::invalid_argument("timeout appears more than once");
        auto& duration = option == "--idle-timeout-ms"
                             ? options.timeouts.idle
                             : options.timeouts.keep_alive;
        duration = ParseConnectionTimeout(value);
        seen = true;
      } else {
        if (has_root) {
          throw std::invalid_argument("--root appears more than once");
        }
        if (value.empty()) {
          throw std::invalid_argument("root directory is empty");
        }
        options.root = value;
        has_root = true;
      }
      continue;
    }
    throw std::invalid_argument("unknown option");
  }
  if (!has_port || !has_root) {
    throw std::invalid_argument(
        "exactly one --port and one --root are required");
  }
  return options;
}

struct ShutdownSignalHandler {
  hp::app::SignalWatcher& signals;
  hp::net::TcpServer& server;
  std::chrono::milliseconds shutdown_timeout;
  bool draining{false};

  void HandleShutdownSignal(std::uint32_t) {
    while (const int signal = signals.ReadNextSignal()) {
      if (draining) {
        server.ForceShutdown();
      } else {
        draining = true;
        server.RequestGracefulShutdown(hp::timer::TimerQueue::Clock::now() +
                                       shutdown_timeout);
      }
      hp::base::info("Shutdown signal observed: " + std::to_string(signal) +
                     ".");
    }
  }
};

int RunServer(int argc, char* argv[]) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    PrintUsage(std::cout);
    return 0;
  }

  ServerOptions options;
  try {
    options = ParseServerOptions(argc, argv);
  } catch (const std::invalid_argument& error) {
    std::cerr << "Error: " << error.what() << ".\n";
    PrintUsage(std::cerr);
    return 2;
  }

  ShutdownSignalMask mask;
  hp::base::LoggerSession logging;
  try {
    hp::http::StaticFileService service(options.root);
    hp::app::SignalWatcher signals;
    hp::net::TcpServer server(options.port,
                              hp::http::kMaxRequestBytes,
                              options.threads,
                              options.timeouts);
    server.set_CreateMessageCallback_callback(
        std::bind_front(&hp::app::HttpMessageFactory::CreateMessageCallback,
                        hp::app::HttpMessageFactory{service}));
    ShutdownSignalHandler shutdown_signals{signals,
                                           server,
                                           options.shutdown_timeout};
    auto& signal_channel = server.WatchControlFd(signals.fd());
    signal_channel.set_HandleShutdownSignal_callback(
        std::bind_front(&ShutdownSignalHandler::HandleShutdownSignal,
                        &shutdown_signals));
    signal_channel.set_interest(EPOLLIN);
    const std::string port_text = std::to_string(server.bound_port());
    hp::base::info("HP HTTP Server V0.1 / S3 minimal HTTP static file server");
    hp::base::info("Listening on TCP port " + port_text + ".");
    std::cout << "V0.1 / S3 minimal HTTP static file server listening on port "
              << port_text << "." << std::endl;
    server.Run();
    return 0;
  } catch (const std::exception& error) {
    hp::base::error(error.what());
  } catch (...) {
    hp::base::error("Unknown fatal error.");
  }
  return 1;
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    return RunServer(argc, argv);
  } catch (const std::exception& error) {
    hp::base::error(error.what());
  } catch (...) {
    hp::base::error("Unknown fatal error.");
  }
  return 1;
}
