#pragma once
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>

#include "ManagementHandler.h"

namespace hp::app {
class ManagementServer {
 public:
  using FailedCallback = std::function<void()>;
  ManagementServer(std::uint16_t port,
                   std::atomic<Lifecycle>& lifecycle,
                   const metrics::ServerMetrics& metrics,
                   const base::ResourceBudget& businessBudget,
                   const base::LoggerSession& logging);
  ~ManagementServer();
  ManagementServer(const ManagementServer&) = delete;
  ManagementServer& operator=(const ManagementServer&) = delete;
  void setFailedCallback(FailedCallback failedCallback) {
    failedCallback_ = std::move(failedCallback);
  }
  std::uint16_t start();
  void requestStop();
  void join();
  bool failed() const;

 private:
  void runManagement() noexcept;
  void onStarted(net::TcpServer& server);
  void clearControl();
  void recordFailure(std::exception_ptr failure) noexcept;
  const std::uint16_t requestedPort_;
  std::atomic<Lifecycle>& lifecycle_;
  ManagementHandler handler_;
  std::shared_ptr<base::ResourceBudget> budget_;
  FailedCallback failedCallback_;

  mutable std::mutex mutex_;
  std::condition_variable startupChanged_;
  net::TcpServer* server_{nullptr};
  bool attempted_{false}, startupComplete_{false}, startupSucceeded_{false}, stopRequested_{false};
  std::uint16_t boundPort_{};
  std::exception_ptr failure_;
  std::thread managementThread_;
};
}  // namespace hp::app
