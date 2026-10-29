#pragma once

#include <functional>
#include <memory>
#include <optional>

#include "business/BoundedTaskExecutor.h"
#include "http/TaskApiHandler.h"

namespace hp::app {
using TaskExecutionCallback = std::function<http::ResponseResult(const http::TaskApiHandler&,
                                                                 http::TaskRoute,
                                                                 const http::HttpRequest&,
                                                                 http::ConnectionPolicy)>;

enum class AsyncDispatch { kSynchronous, kImmediate, kPending };

struct AsyncTaskSubmission {
  AsyncDispatch dispatch;
  std::optional<http::ResponseResult> response{};
  std::shared_ptr<business::TaskJob> job{};
};

class AsyncTaskHandler final {
 public:
  AsyncTaskHandler(http::TaskApiHandler handler,
                   std::shared_ptr<business::BoundedTaskExecutor> executor,
                   TaskExecutionCallback execute = {});
  [[nodiscard]] AsyncTaskSubmission submitRequest(const http::HttpRequest& request,
                                                  http::ConnectionPolicy policy) const;
  [[nodiscard]] std::optional<http::ResponseResult> collectResponse(
      const std::shared_ptr<business::TaskJob>& job) const;
  void cancelRequest(const std::shared_ptr<business::TaskJob>& job) const noexcept;
  [[nodiscard]] static http::ResponseResult timeoutResponse();
  [[nodiscard]] static http::ResponseResult failureResponse();

 private:
  http::TaskApiHandler handler_;
  std::shared_ptr<business::BoundedTaskExecutor> executor_;
  TaskExecutionCallback execute_;
};
}  // namespace hp::app
