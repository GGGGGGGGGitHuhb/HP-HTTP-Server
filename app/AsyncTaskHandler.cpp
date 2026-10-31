#include "AsyncTaskHandler.h"

#include <stdexcept>
#include <utility>

namespace hp::app {
namespace {
class ResponseTaskWork final : public business::TaskWork {
 public:
  ResponseTaskWork(http::TaskApiHandler handler,
                   http::TaskRoute route,
                   http::HttpRequest request,
                   http::ConnectionPolicy policy,
                   TaskExecutionCallback execute);
  void executeTask(const business::TaskCancellation& cancellation) override;
  std::optional<http::ResponseResult> response;

 private:
  http::TaskApiHandler handler_;
  http::TaskRoute route_;
  http::HttpRequest request_;
  http::ConnectionPolicy policy_;
  TaskExecutionCallback execute_;
};

ResponseTaskWork::ResponseTaskWork(http::TaskApiHandler handler,
                                   http::TaskRoute route,
                                   http::HttpRequest request,
                                   http::ConnectionPolicy policy,
                                   TaskExecutionCallback execute)
    : handler_(std::move(handler)),
      route_(route),
      request_(std::move(request)),
      policy_(policy),
      execute_(std::move(execute)) {}

void ResponseTaskWork::executeTask(const business::TaskCancellation& cancellation) {
  if (cancellation.stopRequested()) return;
  try {
    response = execute_ ? execute_(handler_, route_, request_, policy_)
                        : handler_.onTaskResponse(route_, request_, policy_);
  } catch (...) {
    response = AsyncTaskHandler::failureResponse();
  }
}
}  // namespace

AsyncTaskHandler::AsyncTaskHandler(http::TaskApiHandler handler,
                                   std::shared_ptr<business::BoundedTaskExecutor> executor,
                                   TaskExecutionCallback execute)
    : handler_(std::move(handler)), executor_(std::move(executor)), execute_(std::move(execute)) {
  if (!executor_) throw std::invalid_argument("task executor is required");
}

AsyncTaskSubmission AsyncTaskHandler::submitRequest(const http::HttpRequest& request,
                                                    http::ConnectionPolicy policy) const {
  const auto target = http::parseRequestTarget(request.target);
  if (!target.target) return {AsyncDispatch::kSynchronous, {}, {}};
  const auto route = http::classifyTaskRoute(*target.target);
  if (route.kind == http::TaskRouteKind::kOther) return {AsyncDispatch::kSynchronous, {}, {}};
  try {
    if (auto rejected = handler_.prevalidateTask(route, request, policy))
      return {AsyncDispatch::kImmediate, std::move(rejected), {}};
    auto work = std::make_shared<ResponseTaskWork>(handler_, route, request, policy, execute_);
    auto admitted = executor_->submitTask(std::move(work));
    if (admitted.admission != business::TaskAdmission::kAccepted)
      return {AsyncDispatch::kImmediate,
              http::makeTaskJsonResponse(http::Status::kServiceUnavailable,
                                         R"({"error":"task_overloaded"})",
                                         http::ConnectionPolicy::kClose),
              {}};
    return {AsyncDispatch::kPending, {}, std::move(admitted.job)};
  } catch (...) {
    return {AsyncDispatch::kImmediate, failureResponse(), {}};
  }
}

std::optional<http::ResponseResult> AsyncTaskHandler::collectResponse(
    const std::shared_ptr<business::TaskJob>& job) const {
  auto result = executor_->collectTask(job);
  if (result.state == business::TaskJobState::kQueued ||
      result.state == business::TaskJobState::kRunning)
    return {};
  if (!result.work) return failureResponse();
  auto work = std::static_pointer_cast<ResponseTaskWork>(std::move(result.work));
  return work->response ? std::move(work->response)
                        : std::optional<http::ResponseResult>(failureResponse());
}

void AsyncTaskHandler::cancelRequest(const std::shared_ptr<business::TaskJob>& job) const noexcept {
  executor_->cancelTask(job);
}

http::ResponseResult AsyncTaskHandler::timeoutResponse() {
  return http::makeTaskJsonResponse(http::Status::kServiceUnavailable,
                                    R"({"error":"task_timeout"})",
                                    http::ConnectionPolicy::kClose);
}

http::ResponseResult AsyncTaskHandler::failureResponse() {
  return http::makeTaskJsonResponse(
      http::Status::kInternalServerError,
      R"({"error":{"code":"internal_error","message":"Internal error"}})",
      http::ConnectionPolicy::kClose);
}
}  // namespace hp::app
