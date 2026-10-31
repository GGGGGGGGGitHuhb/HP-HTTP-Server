#include <pthread.h>
#include <signal.h>
#include <sys/epoll.h>

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "HttpConnectionHandler.h"
#include "SignalWatcher.h"
#include "base/ResourceBudget.h"

namespace {
using namespace std::chrono_literals;

class BudgetFixture {
 public:
  BudgetFixture(std::shared_ptr<hp::base::ResourceBudget> budget,
                std::shared_ptr<hp::business::BoundedTaskExecutor> executor,
                hp::metrics::ServerMetrics& metrics);
  hp::http::ResponseResult executeTask(const hp::http::TaskApiHandler& handler,
                                       hp::http::TaskRoute route,
                                       const hp::http::HttpRequest& request,
                                       hp::http::ConnectionPolicy policy);
  void releaseGate();
  bool gateReleased() const { return released_; }
  void control();
  void printSnapshot(std::string_view label);
  void printEvent(std::string_view label, unsigned value = 0);

 private:
  std::shared_ptr<hp::base::ResourceBudget> budget_;
  std::shared_ptr<hp::business::BoundedTaskExecutor> executor_;
  hp::metrics::ServerMetrics& metrics_;
  std::mutex gateMutex_, outputMutex_;
  std::condition_variable gateChanged_;
  bool released_{false};
};

BudgetFixture::BudgetFixture(std::shared_ptr<hp::base::ResourceBudget> budget,
                             std::shared_ptr<hp::business::BoundedTaskExecutor> executor,
                             hp::metrics::ServerMetrics& metrics)
    : budget_(std::move(budget)), executor_(std::move(executor)), metrics_(metrics) {}

hp::http::ResponseResult BudgetFixture::executeTask(const hp::http::TaskApiHandler& handler,
                                                    hp::http::TaskRoute route,
                                                    const hp::http::HttpRequest& request,
                                                    hp::http::ConnectionPolicy policy) {
  auto response = handler.onTaskResponse(route, request, policy);
  std::unique_lock lock(gateMutex_);
  printEvent("ENTERED");
  if (!gateChanged_.wait_for(lock, 5s, [this] { return gateReleased(); }))
    throw std::runtime_error("resource fixture gate deadline");
  return response;
}

void BudgetFixture::releaseGate() {
  std::lock_guard lock(gateMutex_);
  released_ = true;
  gateChanged_.notify_all();
}

void BudgetFixture::printEvent(std::string_view label, unsigned value) {
  std::lock_guard lock(outputMutex_);
  std::cout << label << ' ' << value << std::endl;
}

void BudgetFixture::printSnapshot(std::string_view label) {
  const auto resources = budget_->snapshot();
  const auto jobs = executor_->snapshot();
  const auto metrics = metrics_.snapshot();
  std::lock_guard lock(outputMutex_);
  std::cout << label << ' ' << resources.connections.current << ' ' << resources.inputBytes.current
            << ' ' << resources.outputBytes.current << ' ' << resources.connections.peak << ' '
            << resources.inputBytes.peak << ' ' << resources.outputBytes.peak << ' '
            << resources.connections.rejected << ' ' << resources.inputBytes.rejected << ' '
            << resources.outputBytes.rejected << ' ' << jobs.outstanding << ' ' << jobs.admitted
            << ' ' << jobs.released << ' ' << metrics.requestsStarted << ' '
            << metrics.responsesCompleted << ' ' << metrics.requestsAborted << ' '
            << metrics.parseErrors << ' ' << metrics.providerErrors << std::endl;
}

void BudgetFixture::control() {
  std::string command;
  while (std::getline(std::cin, command)) {
    if (command == "snapshot")
      printSnapshot("SNAP");
    else if (command == "release")
      releaseGate();
    else if (command == "quit")
      break;
  }
  releaseGate();
}

struct Shutdown {
  hp::app::SignalWatcher& signals;
  hp::net::TcpServer& server;
  hp::business::BoundedTaskExecutor& executor;
  bool draining{false};
  void onSignal(std::uint32_t);
};

void Shutdown::onSignal(std::uint32_t) {
  while (signals.readNextSignal()) {
    if (draining)
      server.requestServerForceClose();
    else {
      draining = true;
      executor.stopAcceptingAndCancel();
      server.requestServerGracefulShutdown(hp::timer::TimerQueue::Clock::now() + 100ms);
    }
  }
}

hp::base::ResourceLimits fixtureLimits(std::string_view mode) {
  using namespace hp::base;
  if (mode == "connections") return {2, 64U * 1024U * 1024U, 64U * 1024U * 1024U};
  if (mode == "input")
    return {4, 4 * kTransportInputBytes + kRequestInputBytes, 64U * 1024U * 1024U};
  if (mode == "output") return {8, 64U * 1024U * 1024U, 9U * 1024U * 1024U};
  if (mode == "interim-output") return {8, 64U * 1024U * 1024U, 1};
  if (mode == "async") return {8, 64U * 1024U * 1024U, 64U * 1024U * 1024U};
  throw std::invalid_argument("unknown fixture mode");
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) return 2;
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGTERM);
  sigaddset(&mask, SIGINT);
  if (pthread_sigmask(SIG_BLOCK, &mask, nullptr)) return 2;
  auto budget = std::make_shared<hp::base::ResourceBudget>(fixtureLimits(argv[3]));
  auto executor = std::make_shared<hp::business::BoundedTaskExecutor>(
      hp::business::TaskExecutorOptions{2, 32, 200ms});
  hp::metrics::ServerMetrics metrics;
  BudgetFixture fixture(budget, executor, metrics);
  int result = 0;
  {
    hp::http::StaticFileService files(argv[1]);
    hp::http::RequestRouter router(files, std::make_shared<hp::business::TaskService>());
    std::shared_ptr<hp::app::AsyncTaskHandler> adapter;
    if (std::string_view(argv[3]) == "async") {
      adapter = std::make_shared<hp::app::AsyncTaskHandler>(
          router.taskHandler(),
          executor,
          [&fixture](const hp::http::TaskApiHandler& handler,
                     hp::http::TaskRoute route,
                     const hp::http::HttpRequest& request,
                     hp::http::ConnectionPolicy policy) {
            return fixture.executeTask(handler, route, request, policy);
          });
    }
    hp::app::SignalWatcher signals;
    hp::net::TcpServer server(0,
                              hp::base::kTransportInputBytes,
                              std::stoi(argv[2]),
                              {},
                              &metrics,
                              budget);
    server.registerMessageFactoryCallback(
        [factory =
             hp::app::HttpMessageFactory{router, &metrics, false, 500ms, {}, adapter, budget}] {
          return factory.onMessageFactory();
        });
    Shutdown shutdown{signals, server, *executor};
    auto& channel = server.watchControlFd(signals.fd());
    channel.registerEventCallback([&shutdown](std::uint32_t events) { shutdown.onSignal(events); });
    channel.setInterest(EPOLLIN);
    std::thread controller(&BudgetFixture::control, &fixture);
    fixture.printEvent("PORT", server.boundPort());
    try {
      server.runTcpServer();
    } catch (...) {
      result = 1;
    }
    fixture.printEvent("NETWORK_STOPPED");
    executor->stopAcceptingAndCancel();
    controller.join();
    executor->joinWorkers();
  }
  fixture.printSnapshot("FINAL");
  const auto state = budget->snapshot();
  return result || state.connections.current || state.inputBytes.current ||
         state.outputBytes.current || executor->snapshot().outstanding;
}
