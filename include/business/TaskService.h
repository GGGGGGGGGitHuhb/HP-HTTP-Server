#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace hp::business {
using TaskId = std::uint64_t;
inline constexpr std::size_t kMaxTasks = 128;

struct TaskInput {
  std::string title;
  bool completed{false};
};

struct Task {
  TaskId id{0};
  TaskInput input;
};

enum class TaskStatus { kSuccess, kNotFound, kCapacity, kIdExhausted };

struct TaskResult {
  TaskStatus status;
  std::optional<Task> task{};
};

class TaskService final {
 public:
  [[nodiscard]] TaskResult createTask(const TaskInput& input);
  [[nodiscard]] TaskResult getTask(TaskId id) const;
  [[nodiscard]] std::vector<Task> listTasks() const;
  [[nodiscard]] TaskResult replaceTask(TaskId id, const TaskInput& input);
  [[nodiscard]] TaskResult deleteTask(TaskId id);

 private:
  friend struct TaskServiceTestAccess;

  mutable std::mutex mutex_;
  std::map<TaskId, Task> tasks_;
  TaskId nextId_{1};
  bool exhausted_{false};
};
}  // namespace hp::business
