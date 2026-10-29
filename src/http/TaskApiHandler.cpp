#include "http/TaskApiHandler.h"

#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>

namespace hp::http {
namespace {
using Json = nlohmann::json;

std::string_view trimOws(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

bool equalAscii(std::string_view text, std::string_view expected) {
  if (text.size() != expected.size()) return false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    char byte = text[i];
    if (byte >= 'A' && byte <= 'Z') byte += 'a' - 'A';
    if (byte != expected[i]) return false;
  }
  return true;
}

class TaskInputSax final : public nlohmann::json_sax<Json> {
 public:
  bool null() override;
  bool boolean(bool value) override;
  bool number_integer(number_integer_t) override;
  bool number_unsigned(number_unsigned_t) override;
  bool number_float(number_float_t, const string_t&) override;
  bool string(string_t& value) override;
  bool binary(binary_t&) override;
  bool start_object(std::size_t) override;
  bool key(string_t& value) override;
  bool end_object() override;
  bool start_array(std::size_t) override;
  bool end_array() override;
  bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override;

  [[nodiscard]] bool complete() const { return ended_; }
  [[nodiscard]] business::TaskInput takeInput() { return std::move(input_); }

 private:
  enum class Field { kNone, kTitle, kCompleted };

  business::TaskInput input_;
  Field pending_{Field::kNone};
  bool started_{false}, ended_{false}, titleSeen_{false}, completedSeen_{false};
};

bool TaskInputSax::null() { return false; }

bool TaskInputSax::boolean(bool value) {
  if (pending_ != Field::kCompleted || ended_) return false;
  input_.completed = value;
  pending_ = Field::kNone;
  return true;
}

bool TaskInputSax::number_integer(number_integer_t) { return false; }

bool TaskInputSax::number_unsigned(number_unsigned_t) { return false; }

bool TaskInputSax::number_float(number_float_t, const string_t&) { return false; }

bool TaskInputSax::string(string_t& value) {
  if (pending_ != Field::kTitle || ended_ || value.empty() || value.size() > 256) return false;
  for (unsigned char byte : value)
    if (byte < 0x20 || byte == 0x7f) return false;
  input_.title = std::move(value);
  pending_ = Field::kNone;
  return true;
}

bool TaskInputSax::binary(binary_t&) { return false; }

bool TaskInputSax::start_object(std::size_t) {
  if (started_) return false;
  started_ = true;
  return true;
}

bool TaskInputSax::key(string_t& value) {
  if (!started_ || ended_ || pending_ != Field::kNone) return false;
  if (value == "title" && !titleSeen_) {
    titleSeen_ = true;
    pending_ = Field::kTitle;
    return true;
  }
  if (value == "completed" && !completedSeen_) {
    completedSeen_ = true;
    pending_ = Field::kCompleted;
    return true;
  }
  return false;
}

bool TaskInputSax::end_object() {
  if (!started_ || ended_ || pending_ != Field::kNone || !titleSeen_ || !completedSeen_)
    return false;
  ended_ = true;
  return true;
}

bool TaskInputSax::start_array(std::size_t) { return false; }

bool TaskInputSax::end_array() { return false; }

bool TaskInputSax::parse_error(std::size_t,
                               const std::string&,
                               const nlohmann::detail::exception&) {
  return false;
}

std::string serializeTaskResult(const business::Task& task) {
  return Json{{"id", std::to_string(task.id)},
              {"title", task.input.title},
              {"completed", task.input.completed}}
      .dump();
}

std::string serializeTasks(const std::vector<business::Task>& tasks) {
  Json array = Json::array();
  for (const auto& task : tasks)
    array.push_back({{"id", std::to_string(task.id)},
                     {"title", task.input.title},
                     {"completed", task.input.completed}});
  return Json{{"tasks", std::move(array)}}.dump();
}

ResponseResult taskResponse(Status status,
                            std::string_view body,
                            ConnectionPolicy policy,
                            bool omitBody = false,
                            std::string_view allow = {}) {
  if (body.size() > kMaxTaskResponseBytes) throw std::length_error("task response budget");
  return makeTaskJsonResponse(status, body, policy, omitBody, allow);
}

ResponseResult taskError(Status status,
                         std::string_view code,
                         std::string_view message,
                         ConnectionPolicy policy,
                         bool omitBody = false,
                         std::string_view allow = {}) {
  if (status == Status::kMethodNotAllowed || status == Status::kContentTooLarge ||
      status == Status::kInternalServerError)
    policy = ConnectionPolicy::kClose;
  return taskResponse(status,
                      Json{{"error", {{"code", code}, {"message", message}}}}.dump(),
                      policy,
                      omitBody,
                      allow);
}

ResponseResult internalError(bool omitBody) {
  return makeTaskJsonResponse(Status::kInternalServerError,
                              R"({"error":{"code":"internal_error","message":"Internal error"}})",
                              ConnectionPolicy::kClose,
                              omitBody);
}

ResponseResult serviceResponse(const business::TaskResult& result,
                               Status success,
                               ConnectionPolicy policy) {
  switch (result.status) {
    case business::TaskStatus::kNotFound:
      return taskError(Status::kNotFound, "task_not_found", "Task not found", policy);
    case business::TaskStatus::kCapacity:
      return taskError(Status::kServiceUnavailable,
                       "capacity_exhausted",
                       "Task capacity exhausted",
                       policy);
    case business::TaskStatus::kIdExhausted:
      return taskError(Status::kServiceUnavailable,
                       "id_exhausted",
                       "Task identifiers exhausted",
                       policy);
    case business::TaskStatus::kSuccess:
      return taskResponse(success, serializeTaskResult(*result.task), policy);
  }
  return internalError(false);
}
}  // namespace

TaskRoute classifyTaskRoute(const RequestTarget& target) {
  if (target.path() != "/api/tasks" && !target.path().starts_with("/api/tasks/"))
    return {TaskRouteKind::kOther, 0};
  TaskRoute result{TaskRouteKind::kCollection, 0};
  if (target.components().size() > 3) return {TaskRouteKind::kNotFound, 0};
  if (target.components().size() == 3) {
    const auto& digits = target.components().back();
    if (digits.empty() || digits.front() == '0') return {TaskRouteKind::kInvalidId, 0};
    for (char byte : digits) {
      if (byte < '0' || byte > '9') return {TaskRouteKind::kInvalidId, 0};
      const auto digit = static_cast<std::uint64_t>(byte - '0');
      if (result.id > (std::numeric_limits<business::TaskId>::max() - digit) / 10)
        return {TaskRouteKind::kInvalidId, 0};
      result.id = result.id * 10 + digit;
    }
    result.kind = TaskRouteKind::kItem;
  }
  if (!target.queryParameters().empty()) return {TaskRouteKind::kUnsupportedQuery, 0};
  return result;
}

bool isTaskMediaType(const ContentTypeField& field) {
  if (!field.present || field.duplicate) return false;
  const auto value = trimOws(field.value);
  const auto separator = value.find(';');
  if (!equalAscii(trimOws(value.substr(0, separator)), "application/json")) return false;
  if (separator == std::string_view::npos) return true;
  const auto parameter = trimOws(value.substr(separator + 1));
  const auto equal = parameter.find('=');
  if (equal == std::string_view::npos ||
      !equalAscii(trimOws(parameter.substr(0, equal)), "charset"))
    return false;
  auto charset = trimOws(parameter.substr(equal + 1));
  if (charset.size() >= 2 && charset.front() == '"' && charset.back() == '"')
    charset = charset.substr(1, charset.size() - 2);
  return equalAscii(charset, "utf-8");
}

std::optional<business::TaskInput> parseTaskInput(std::string_view body) {
  if (body.size() > kMaxTaskInputBytes || body.find('\0') != std::string_view::npos ||
      body.starts_with("\xef\xbb\xbf"))
    return std::nullopt;
  TaskInputSax parser;
  if (!Json::sax_parse(body.begin(),
                       body.end(),
                       &parser,
                       Json::input_format_t::json,
                       true,
                       false) ||
      !parser.complete())
    return std::nullopt;
  return parser.takeInput();
}

TaskApiHandler::TaskApiHandler(std::shared_ptr<business::TaskService> service)
    : service_(std::move(service)) {
  if (!service_) throw std::invalid_argument("TaskService is required");
}

std::optional<ResponseResult> TaskApiHandler::validateTaskRequest(TaskRoute route,
                                                                  std::string_view method,
                                                                  ConnectionPolicy policy) const {
  const bool head = method == "HEAD";
  try {
    if (route.kind == TaskRouteKind::kInvalidId)
      return taskError(Status::kBadRequest, "invalid_id", "Invalid task identifier", policy, head);
    if (route.kind == TaskRouteKind::kNotFound || route.kind == TaskRouteKind::kOther)
      return taskError(Status::kNotFound, "route_not_found", "Route not found", policy, head);
    if (route.kind == TaskRouteKind::kUnsupportedQuery)
      return taskError(Status::kBadRequest,
                       "unsupported_query",
                       "Query parameters are not supported",
                       policy,
                       head);
    const bool collection = route.kind == TaskRouteKind::kCollection;
    if (method != "GET" && (collection ? method != "POST" : method != "PUT" && method != "DELETE"))
      return taskError(Status::kMethodNotAllowed,
                       "method_not_allowed",
                       "Method not allowed",
                       policy,
                       head,
                       collection ? "GET, POST" : "GET, PUT, DELETE");
    return std::nullopt;
  } catch (...) {
    return internalError(head);
  }
}

std::optional<ResponseResult> TaskApiHandler::onTaskHeaders(TaskRoute route,
                                                            std::string_view method,
                                                            ConnectionPolicy) const {
  return validateTaskRequest(route, method, ConnectionPolicy::kClose);
}

ResponseResult TaskApiHandler::executeTask(TaskRoute route,
                                           const HttpRequest& request,
                                           ConnectionPolicy policy) const {
  if (request.body.size() > kMaxTaskInputBytes)
    return taskError(Status::kContentTooLarge,
                     "body_too_large",
                     "Task body exceeds 8192 bytes",
                     policy);
  if (request.method == "GET" || request.method == "DELETE") {
    if (!request.body.empty())
      return taskError(Status::kBadRequest,
                       "unexpected_body",
                       "Request body is not allowed",
                       policy);
    if (request.method == "DELETE") {
      const auto deleted = service_->deleteTask(route.id);
      if (deleted.status != business::TaskStatus::kSuccess)
        return serviceResponse(deleted, Status::kOk, policy);
      return taskResponse(Status::kOk,
                          Json{{"id", std::to_string(route.id)}, {"deleted", true}}.dump(),
                          policy);
    }
    if (route.kind == TaskRouteKind::kCollection)
      return taskResponse(Status::kOk, serializeTasks(service_->listTasks()), policy);
    return serviceResponse(service_->getTask(route.id), Status::kOk, policy);
  }
  if (!isTaskMediaType(request.contentType))
    return taskError(Status::kUnsupportedMediaType,
                     "unsupported_media_type",
                     "Content-Type must be application/json",
                     policy);
  const auto input = parseTaskInput(request.body);
  if (!input) return taskError(Status::kBadRequest, "invalid_json", "Invalid task JSON", policy);
  if (request.method == "POST")
    return serviceResponse(service_->createTask(*input), Status::kCreated, policy);
  return serviceResponse(service_->replaceTask(route.id, *input), Status::kOk, policy);
}

ResponseResult TaskApiHandler::onTaskResponse(TaskRoute route,
                                              const HttpRequest& request,
                                              ConnectionPolicy policy) const {
  try {
    if (auto rejected = validateTaskRequest(route, request.method, policy))
      return std::move(*rejected);
    return executeTask(route, request, policy);
  } catch (...) {
    return internalError(request.method == "HEAD");
  }
}
}  // namespace hp::http
