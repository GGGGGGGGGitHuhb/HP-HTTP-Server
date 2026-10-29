#pragma once

#include <memory>
#include <optional>

#include "business/TaskService.h"
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include "http/RequestTarget.h"

namespace hp::http {
inline constexpr std::size_t kMaxTaskInputBytes = 8192;
inline constexpr std::size_t kMaxTaskResponseBytes = 262144;

enum class TaskRouteKind { kOther, kCollection, kItem, kInvalidId, kNotFound, kUnsupportedQuery };

struct TaskRoute {
  TaskRouteKind kind{TaskRouteKind::kOther};
  business::TaskId id{0};
};

[[nodiscard]] TaskRoute classifyTaskRoute(const RequestTarget& target);
[[nodiscard]] std::optional<business::TaskInput> parseTaskInput(std::string_view body);
[[nodiscard]] bool isTaskMediaType(const ContentTypeField& field);

class TaskApiHandler final {
 public:
  explicit TaskApiHandler(std::shared_ptr<business::TaskService> service);

  [[nodiscard]] std::optional<ResponseResult> onTaskHeaders(TaskRoute route,
                                                            std::string_view method,
                                                            ConnectionPolicy policy) const;
  [[nodiscard]] ResponseResult onTaskResponse(TaskRoute route,
                                              const HttpRequest& request,
                                              ConnectionPolicy policy) const;

  [[nodiscard]] std::optional<ResponseResult> prevalidateTask(TaskRoute route,
                                                              const HttpRequest& request,
                                                              ConnectionPolicy policy) const;

 private:
  [[nodiscard]] std::optional<ResponseResult> validateTaskRequest(TaskRoute route,
                                                                  std::string_view method,
                                                                  ConnectionPolicy policy) const;

  [[nodiscard]] ResponseResult executeTask(TaskRoute route,
                                           const HttpRequest& request,
                                           ConnectionPolicy policy) const;

  std::shared_ptr<business::TaskService> service_;
};
}  // namespace hp::http
