#include "business/TaskService.h"

#include <limits>
#include <utility>

namespace hp::business {
TaskResult TaskService::createTask(const TaskInput& input) {
  Task task{0, input};
  TaskResult result{TaskStatus::kSuccess, task};
  std::lock_guard lock(mutex_);
  if (tasks_.size() == kMaxTasks) return {TaskStatus::kCapacity, {}};
  if (exhausted_) return {TaskStatus::kIdExhausted, {}};
  task.id = result.task->id = nextId_;
  // 返回值的复制先完成；插入成功后才推进ID，分配异常不留下部分状态。
  tasks_.emplace(task.id, std::move(task));
  if (nextId_ == std::numeric_limits<TaskId>::max())
    exhausted_ = true;
  else
    ++nextId_;
  return result;
}

TaskResult TaskService::getTask(TaskId id) const {
  std::lock_guard lock(mutex_);
  const auto found = tasks_.find(id);
  if (found == tasks_.end()) return {TaskStatus::kNotFound, {}};
  return {TaskStatus::kSuccess, found->second};
}

std::vector<Task> TaskService::listTasks() const {
  std::lock_guard lock(mutex_);
  std::vector<Task> snapshot;
  snapshot.reserve(tasks_.size());
  for (const auto& [id, task] : tasks_) snapshot.push_back(task);
  return snapshot;
}

TaskResult TaskService::replaceTask(TaskId id, const TaskInput& input) {
  Task replacement{id, input};
  TaskResult result{TaskStatus::kSuccess, replacement};
  std::lock_guard lock(mutex_);
  const auto found = tasks_.find(id);
  if (found == tasks_.end()) return {TaskStatus::kNotFound, {}};
  found->second.input.title.swap(replacement.input.title);
  found->second.input.completed = replacement.input.completed;
  return result;
}

TaskResult TaskService::deleteTask(TaskId id) {
  std::lock_guard lock(mutex_);
  const auto found = tasks_.find(id);
  if (found == tasks_.end()) return {TaskStatus::kNotFound, {}};
  tasks_.erase(found);
  return {TaskStatus::kSuccess, {}};
}
}  // namespace hp::business
