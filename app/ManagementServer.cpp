#include "ManagementServer.h"

#include <stdexcept>

namespace hp::app {
ManagementServer::ManagementServer(std::uint16_t port,
                                   std::atomic<Lifecycle>& lifecycle,
                                   const metrics::ServerMetrics& metrics,
                                   const base::ResourceBudget& businessBudget,
                                   const base::LoggerSession& logging)
    : requestedPort_(port),
      lifecycle_(lifecycle),
      handler_(lifecycle, metrics, businessBudget, logging),
      budget_(std::make_shared<base::ResourceBudget>(
          base::ResourceLimits{8, 16U * 1024U * 1024U, 256U * 1024U})) {}

ManagementServer::~ManagementServer() {
  requestStop();
  join();
}

std::uint16_t ManagementServer::start() {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
  std::unique_lock lock(mutex_);
  if (attempted_) throw std::logic_error("management cannot restart");
  attempted_ = true;
  try {
    managementThread_ = std::thread(&ManagementServer::runManagement, this);
  } catch (...) {
    lock.unlock();
    recordFailure(std::current_exception());
    throw;
  }
  if (!startupChanged_.wait_until(lock, deadline, [this] { return startupComplete_; })) {
    stopRequested_ = true;
    if (server_) server_->requestServerStop();
    lock.unlock();
    const auto failure = std::make_exception_ptr(std::runtime_error("management startup timeout"));
    recordFailure(failure);
    join();
    std::rethrow_exception(failure);
  }
  if (!startupSucceeded_) {
    const auto failure = failure_;
    lock.unlock();
    join();
    if (failure) std::rethrow_exception(failure);
    throw std::runtime_error("management startup stopped");
  }
  return boundPort_;
}

void ManagementServer::requestStop() {
  std::lock_guard lock(mutex_);
  stopRequested_ = true;
  if (server_) server_->requestServerStop();
}

void ManagementServer::join() {
  if (managementThread_.joinable()) managementThread_.join();
}

bool ManagementServer::failed() const {
  std::lock_guard lock(mutex_);
  return static_cast<bool>(failure_);
}

void ManagementServer::clearControl() {
  std::lock_guard lock(mutex_);
  server_ = nullptr;
}

void ManagementServer::onStarted(net::TcpServer& server) {
  std::lock_guard lock(mutex_);
  if (stopRequested_) {
    server.requestServerStop();
    return;
  }
  boundPort_ = server.boundPort();
  startupSucceeded_ = true;
  startupComplete_ = true;
  startupChanged_.notify_all();
}

void ManagementServer::recordFailure(std::exception_ptr failure) noexcept {
  lifecycle_ = Lifecycle::kFailed;
  {
    std::lock_guard lock(mutex_);
    failure_ = failure;
    startupComplete_ = true;
    startupChanged_.notify_all();
  }
  try {
    if (failedCallback_) failedCallback_();
  } catch (...) {
  }
}

void ManagementServer::runManagement() noexcept {
  try {
    net::TcpServer server(requestedPort_,
                          base::kTransportInputBytes,
                          0,
                          {std::chrono::milliseconds{2000}, std::chrono::milliseconds{0}},
                          nullptr,
                          budget_,
                          net::ListenAddress::kLoopback);
    // 指针只在持锁期间被停止调用借用，先清除再析构owner。
    struct ControlGuard {
      ManagementServer& owner;
      ~ControlGuard() { owner.clearControl(); }
    } guard{*this};
    {
      std::lock_guard lock(mutex_);
      server_ = &server;
      if (stopRequested_) server.requestServerStop();
    }
    server.registerMessageFactoryCallback([this] { return handler_.onMessageFactory(budget_); });
    server.setStartedCallback([this, &server] { onStarted(server); });
    server.runTcpServer();
    std::lock_guard lock(mutex_);
    startupComplete_ = true;
    startupChanged_.notify_all();
  } catch (...) {
    recordFailure(std::current_exception());
  }
}
}  // namespace hp::app
