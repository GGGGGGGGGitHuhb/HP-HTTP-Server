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

#include "HttpConnectionHandler.h"
#include "SignalWatcher.h"
#include "base/AsyncLogger.h"
#include "base/Logger.h"
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include "http/StaticFileService.h"
#include "net/TcpServer.h"

namespace {

// 日志器先于 SignalWatcher 启动，因此每个后台线程都必须
// 已经继承关闭信号的屏蔽掩码。仅在日志线程 join 后恢复。
class ShutdownSignalMask {
 public:
  ShutdownSignalMask() {
    sigset_t signals;
    ::sigemptyset(&signals);
    ::sigaddset(&signals, SIGINT);
    ::sigaddset(&signals, SIGTERM);
    const int result = ::pthread_sigmask(SIG_BLOCK, &signals, &previous_);
    if (result)
      throw std::system_error(result, std::generic_category(), "block logger shutdown signals");
  }

  ~ShutdownSignalMask() { ::pthread_sigmask(SIG_SETMASK, &previous_, nullptr); }

 private:
  sigset_t previous_{};
};

void printUsage(std::ostream& output) {
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

[[nodiscard]] std::uint16_t parsePort(std::string_view text) {
  unsigned int value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 10);
  if (text.empty() || error != std::errc{} || end != text.data() + text.size() || value > 65535U) {
    throw std::invalid_argument("port must be a decimal value in 0-65535");
  }
  return static_cast<std::uint16_t>(value);
}

[[nodiscard]] std::size_t parseWorkerCount(std::string_view value) {
  unsigned int count = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), count, 10);
  if (value.empty() || error != std::errc{} || end != value.data() + value.size() || count > 64)
    throw std::invalid_argument("threads must be decimal in 0-64");
  return count;
}

[[nodiscard]] std::chrono::milliseconds parseShutdownTimeout(std::string_view value) {
  unsigned int milliseconds = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), milliseconds, 10);
  if (value.empty() || error != std::errc{} || end != value.data() + value.size() ||
      milliseconds > 60000)
    throw std::invalid_argument("shutdown timeout must be decimal in 0-60000ms");
  return std::chrono::milliseconds(milliseconds);
}

[[nodiscard]] std::chrono::milliseconds parseConnectionTimeout(std::string_view value) {
  unsigned int valueMs = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), valueMs, 10);
  if (value.empty() || error != std::errc{} || end != value.data() + value.size() ||
      valueMs > 86400000)
    throw std::invalid_argument("timeout must be decimal in 0-86400000ms");
  return std::chrono::milliseconds(valueMs);
}

struct ServerOptions {
  std::uint16_t port{0};
  std::string root;
  std::size_t threads{2};
  std::chrono::milliseconds shutdownTimeout{5000};
  hp::net::ConnectionTimeouts timeouts{std::chrono::milliseconds(30000),
                                       std::chrono::milliseconds(15000)};
};

[[nodiscard]] ServerOptions parseServerOptions(int argc, char* argv[]) {
  bool hasPort = false;
  bool hasRoot = false;
  bool hasThreads = false;
  bool hasIdleTimeout = false, hasKeepAliveTimeout = false, hasShutdownTimeout = false;
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
        if (hasPort) {
          throw std::invalid_argument("--port appears more than once");
        }
        options.port = parsePort(value);
        hasPort = true;
      } else if (option == "--threads") {
        if (hasThreads) throw std::invalid_argument("--threads appears more than once");
        options.threads = parseWorkerCount(value);
        hasThreads = true;
      } else if (option == "--shutdown-timeout-ms") {
        if (hasShutdownTimeout)
          throw std::invalid_argument("shutdown timeout appears more than once");
        options.shutdownTimeout = parseShutdownTimeout(value);
        hasShutdownTimeout = true;
      } else if (option == "--idle-timeout-ms" || option == "--keep-alive-timeout-ms") {
        bool& seen = option == "--idle-timeout-ms" ? hasIdleTimeout : hasKeepAliveTimeout;
        if (seen) throw std::invalid_argument("timeout appears more than once");
        auto& duration =
            option == "--idle-timeout-ms" ? options.timeouts.idle : options.timeouts.keepAlive;
        duration = parseConnectionTimeout(value);
        seen = true;
      } else {
        if (hasRoot) {
          throw std::invalid_argument("--root appears more than once");
        }
        if (value.empty()) {
          throw std::invalid_argument("root directory is empty");
        }
        options.root = value;
        hasRoot = true;
      }
      continue;
    }
    throw std::invalid_argument("unknown option");
  }
  if (!hasPort || !hasRoot) {
    throw std::invalid_argument("exactly one --port and one --root are required");
  }
  return options;
}

struct ShutdownSignalHandler {
  hp::app::SignalWatcher& signals;
  hp::net::TcpServer& server;
  std::chrono::milliseconds shutdownTimeout;
  bool draining{false};

  void handleShutdownSignal(std::uint32_t) {
    while (const int signal = signals.readNextSignal()) {
      if (draining) {
        // 再次请求强制关闭
        server.requestServerForceClose();
      } else {
        // 首次请求优雅关闭
        draining = true;
        server.requestServerGracefulShutdown(hp::timer::TimerQueue::Clock::now() + shutdownTimeout);
      }
      hp::base::info("Shutdown signal observed: " + std::to_string(signal) + ".");
    }
  }
};

int runServer(int argc, char* argv[]) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    printUsage(std::cout);
    return 0;
  }

  ServerOptions options;
  try {
    options = parseServerOptions(argc, argv);
  } catch (const std::invalid_argument& error) {
    std::cerr << "Error: " << error.what() << ".\n";
    printUsage(std::cerr);
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
    server.registerMessageFactoryCallback(
        [target = hp::app::HttpMessageFactory{service}]() { return target.onMessageFactory(); });
    ShutdownSignalHandler shutdownSignals{signals, server, options.shutdownTimeout};
    auto& signalChannel = server.watchControlFd(signals.fd());
    signalChannel.registerEventCallback([target = &shutdownSignals](std::uint32_t events) {
      target->handleShutdownSignal(events);
    });
    signalChannel.setInterest(EPOLLIN);
    const std::string portText = std::to_string(server.boundPort());
    hp::base::info("HP HTTP Server V0.1 / S3 minimal HTTP static file server");
    hp::base::info("Listening on TCP port " + portText + ".");
    std::cout << "V0.1 / S3 minimal HTTP static file server listening on port " << portText << "."
              << std::endl;
    server.runTcpServer();
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
    return runServer(argc, argv);
  } catch (const std::exception& error) {
    hp::base::error(error.what());
  } catch (...) {
    hp::base::error("Unknown fatal error.");
  }
  return 1;
}
