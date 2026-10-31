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
#include "ManagementServer.h"
#include "ServerConfig.h"
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

void onBusinessStarted(std::atomic<hp::app::Lifecycle>& lifecycle) {
  auto expected = hp::app::Lifecycle::kStarting;
  lifecycle.compare_exchange_strong(expected, hp::app::Lifecycle::kReady);
}

void onManagementFailed(std::atomic<hp::app::Lifecycle>& lifecycle, hp::net::TcpServer& server) {
  lifecycle = hp::app::Lifecycle::kFailed;
  server.requestServerStop();
}

struct ShutdownSignalHandler {
  hp::app::SignalWatcher& signals;
  hp::net::TcpServer& server;
  hp::business::BoundedTaskExecutor& executor;
  std::chrono::milliseconds shutdownTimeout;
  std::atomic<hp::app::Lifecycle>& lifecycle;
  bool draining{false};

  void handleShutdownSignal(std::uint32_t) {
    while (const int signal = signals.readNextSignal()) {
      if (draining) {
        // 再次请求强制关闭
        server.requestServerForceClose();
      } else {
        // 首次请求优雅关闭
        lifecycle = hp::app::Lifecycle::kStopping;
        draining = true;
        executor.stopAcceptingAndCancel();
        server.requestServerGracefulShutdown(hp::timer::TimerQueue::Clock::now() + shutdownTimeout);
      }
      hp::base::info("Shutdown signal observed: " + std::to_string(signal) + ".");
    }
  }
};

int runServer(int argc, char* argv[]) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    hp::app::printUsage(std::cout);
    return 0;
  }

  hp::app::ServerOptions options;
  try {
    options = hp::app::parseServerOptions(argc, argv);
  } catch (const std::invalid_argument& error) {
    std::cerr << "Error: " << error.what() << ".\n";
    hp::app::printUsage(std::cerr);
    return 2;
  }

  ShutdownSignalMask mask;
  hp::base::LoggerSession logging;
  hp::metrics::ServerMetrics metrics;
  auto resourceBudget = std::make_shared<hp::base::ResourceBudget>(options.resourceLimits);
  std::atomic<hp::app::Lifecycle> lifecycle{hp::app::Lifecycle::kStarting};
  int exitCode = 0;
  try {
    hp::http::StaticFileService service(options.root);
    auto tasks = std::make_shared<hp::business::TaskService>();
    hp::http::RequestRouter router(service, tasks);
    auto executor = std::make_shared<hp::business::BoundedTaskExecutor>();
    auto asyncHandler = std::make_shared<hp::app::AsyncTaskHandler>(router.taskHandler(), executor);
    hp::app::SignalWatcher signals;
    hp::net::TcpServer server(options.port,
                              hp::http::kMaxHeaderBytes,
                              options.threads,
                              options.timeouts,
                              &metrics,
                              resourceBudget);
    server.registerMessageFactoryCallback(
        [target = hp::app::HttpMessageFactory{router,
                                              &metrics,
                                              options.accessLog,
                                              options.requestReadTimeout,
                                              {},
                                              asyncHandler,
                                              resourceBudget}]() {
          return target.onMessageFactory();
        });
    std::unique_ptr<hp::app::ManagementServer> management;
    if (options.adminPort) {
      management = std::make_unique<hp::app::ManagementServer>(*options.adminPort,
                                                               lifecycle,
                                                               metrics,
                                                               *resourceBudget,
                                                               logging);
      management->setFailedCallback(
          [&lifecycle, &server] { onManagementFailed(lifecycle, server); });
    }
    const auto adminPort = management ? management->start() : 0;
    server.setStartedCallback([&lifecycle] { onBusinessStarted(lifecycle); });
    ShutdownSignalHandler shutdownSignals{signals,
                                          server,
                                          *executor,
                                          options.shutdownTimeout,
                                          lifecycle};
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
    if (management) std::cout << "HP_ADMIN_LISTENING 127.0.0.1 " << adminPort << std::endl;
    try {
      server.runTcpServer();
    } catch (...) {
      lifecycle = hp::app::Lifecycle::kFailed;
      throw;
    }
    if (lifecycle != hp::app::Lifecycle::kFailed) lifecycle = hp::app::Lifecycle::kStopping;
    if (management) {
      management->requestStop();
      management->join();
      if (management->failed()) exitCode = 1;
    }
    executor->stopAcceptingAndCancel();
    executor->joinWorkers();
  } catch (const std::exception& error) {
    hp::base::error(error.what());
    exitCode = 1;
  } catch (...) {
    hp::base::error("Unknown fatal error.");
    exitCode = 1;
  }
  logging.stopSessionLogging();
  if (options.metricsOnExit) {
    const auto stats = logging.stats();
    std::cout << "HP_METRICS_BEGIN\n"
              << hp::metrics::ServerMetrics::serializeSnapshot(metrics.snapshot())
              << hp::base::ResourceBudget::serializeSnapshot(resourceBudget->snapshot())
              << "logger_submitted " << stats.submitted << '\n'
              << "logger_accepted " << stats.accepted << '\n'
              << "logger_dropped_full " << stats.droppedFull << '\n'
              << "logger_rejected_stopped " << stats.rejectedStopped << '\n'
              << "logger_truncated " << stats.truncated << '\n'
              << "logger_written " << stats.written << '\n'
              << "logger_failed " << stats.failed << '\n'
              << "logger_pending " << stats.pending << "\nHP_METRICS_END\n"
              << std::flush;
    if (!std::cout) exitCode = 1;
  }
  return exitCode;
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
