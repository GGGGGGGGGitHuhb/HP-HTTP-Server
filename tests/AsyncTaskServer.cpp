#include <pthread.h>
#include <signal.h>
#include <sys/epoll.h>

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

#include "HttpConnectionHandler.h"
#include "SignalWatcher.h"

namespace {
struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  bool released{};
  bool commitFirst{};
  hp::http::ResponseResult execute(const hp::http::TaskApiHandler& handler,
                                   hp::http::TaskRoute route,
                                   const hp::http::HttpRequest& request,
                                   hp::http::ConnectionPolicy policy) {
    std::optional<hp::http::ResponseResult> result;
    if (commitFirst) result = handler.onTaskResponse(route, request, policy);
    {
      std::unique_lock lock(mutex);
      std::cout << "ENTERED" << std::endl;
      if (!changed.wait_for(lock, std::chrono::seconds{5}, [this] { return released; }))
        throw std::runtime_error("test gate release deadline");
    }
    return result ? std::move(*result) : handler.onTaskResponse(route, request, policy);
  }
  void release() {
    std::lock_guard lock(mutex);
    released = true;
    changed.notify_all();
  }
  void control() {
    std::string command;
    std::getline(std::cin, command);
    release();
  }
};
struct Shutdown {
  hp::app::SignalWatcher& signals;
  hp::net::TcpServer& server;
  hp::business::BoundedTaskExecutor& executor;
  bool draining{};
  void onSignal(std::uint32_t) {
    while (signals.readNextSignal()) {
      if (draining)
        server.requestServerForceClose();
      else {
        draining = true;
        executor.stopAcceptingAndCancel();
        server.requestServerGracefulShutdown(hp::timer::TimerQueue::Clock::now() +
                                             std::chrono::milliseconds{100});
      }
    }
  }
};
}  // namespace
int main(int argc, char** argv) {
  if (argc != 5) return 2;
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGTERM);
  sigaddset(&mask, SIGINT);
  if (pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0) return 2;
  hp::http::StaticFileService files(argv[1]);
  hp::http::RequestRouter router(files, std::make_shared<hp::business::TaskService>());
  auto gate = std::make_shared<Gate>();
  gate->commitFirst = std::string_view(argv[4]) == "commit";
  auto executor = std::make_shared<hp::business::BoundedTaskExecutor>(
      hp::business::TaskExecutorOptions{2, 32, std::chrono::milliseconds{std::stoi(argv[3])}});
  auto adapter = std::make_shared<hp::app::AsyncTaskHandler>(
      router.taskHandler(),
      executor,
      [gate](const hp::http::TaskApiHandler& handler,
             hp::http::TaskRoute route,
             const hp::http::HttpRequest& request,
             hp::http::ConnectionPolicy policy) {
        return gate->execute(handler, route, request, policy);
      });
  hp::app::SignalWatcher signals;
  hp::net::TcpServer server(0, hp::http::kMaxHeaderBytes, std::stoi(argv[2]));
  server.registerMessageFactoryCallback(
      [factory = hp::app::HttpMessageFactory{router,
                                             nullptr,
                                             false,
                                             std::chrono::milliseconds{300},
                                             {},
                                             adapter}] { return factory.onMessageFactory(); });
  Shutdown shutdown{signals, server, *executor};
  auto& channel = server.watchControlFd(signals.fd());
  channel.registerEventCallback([&shutdown](std::uint32_t events) { shutdown.onSignal(events); });
  channel.setInterest(EPOLLIN);
  std::thread controller([gate] { gate->control(); });
  std::cout << "PORT " << server.boundPort() << std::endl;
  int code = 0;
  try {
    server.runTcpServer();
    std::cout << "NETWORK_STOPPED" << std::endl;
  } catch (...) {
    gate->release();
    code = 1;
  }
  executor->stopAcceptingAndCancel();
  controller.join();
  executor->joinWorkers();
  auto state = executor->snapshot();
  std::cout << "FINAL " << state.outstanding << ' ' << state.admitted << ' ' << state.released
            << std::endl;
  return code || state.outstanding || state.admitted != state.released;
}
