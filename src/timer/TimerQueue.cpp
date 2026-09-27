#include "timer/TimerQueue.h"

#include <atomic>
#include <cassert>
#include <limits>
#include <stdexcept>
#include <vector>

namespace hp::timer {
namespace {
std::atomic<TimerQueue::Id> nextId{1};

TimerQueue::Id allocateId() {
  auto id = nextId.load(std::memory_order_relaxed);
  do {
    if (!id) throw std::overflow_error("timer id exhausted");
  } while (!nextId.compare_exchange_weak(id,
                                         id == UINT64_MAX ? 0 : id + 1,
                                         std::memory_order_relaxed));
  return id;
}
}  // namespace

TimerQueue::Id TimerQueue::exchangeNextIdForTest(Id value) {
  return nextId.exchange(value);
}

void TimerQueue::requireOwner() const {
  if (owner_ != std::this_thread::get_id())
    throw std::logic_error("TimerQueue wrong owner");
}

TimerQueue::~TimerQueue() noexcept {
  assert(owner_ == std::this_thread::get_id());
  clearTimers();
}

TimerQueue::Id TimerQueue::addTimer(TimePoint deadline, TimerCallback expiryTask) {
  requireOwner();
  if (clearing_) throw std::logic_error("TimerQueue is clearing");
  if (!expiryTask) throw std::invalid_argument("empty timer callback");
  const auto id = allocateId();
  auto [position, inserted] =
      records_.emplace(id, Record{deadline, std::move(expiryTask)});
  (void)inserted;
  try {
    ordered_.emplace(deadline, id);
  } catch (...) {
    auto rejected = records_.extract(position);
    throw;
  }
  lastId_ = id;
  return id;
}

bool TimerQueue::rescheduleTimer(Id id, TimePoint deadline) {
  requireOwner();
  const auto found = records_.find(id);
  if (found == records_.end()) return false;
  if (found->second.deadline == deadline) return true;
  ordered_.emplace(deadline,
                   id);  // 分配失败时保留原有的两个索引。
  ordered_.erase({found->second.deadline, id});
  found->second.deadline = deadline;
  return true;
}

bool TimerQueue::cancelTimer(Id id) {
  requireOwner();
  const auto found = records_.find(id);
  if (found == records_.end()) return false;
  ordered_.erase({found->second.deadline, id});
  auto cancelled = records_.extract(
      found);  // 只有两个索引一致后才销毁捕获对象。
  return true;
}

std::optional<TimerQueue::TimePoint> TimerQueue::nextDeadline() const {
  requireOwner();
  if (ordered_.empty()) return {};
  return ordered_.begin()->first;
}

std::size_t TimerQueue::size() const {
  requireOwner();
  return records_.size();
}

TimerQueue::Id TimerQueue::lastId() const {
  requireOwner();
  return lastId_;
}

void TimerQueue::runDueTimers(TimePoint now, Id cutoff) {
  requireOwner();
  if (running_ || clearing_) throw std::logic_error("recursive timer dispatch");
  std::vector<Id> due;
  for (auto [deadline, id] : ordered_) {
    if (deadline > now) break;
    if (id <= cutoff) due.push_back(id);
  }
  running_ = true;
  try {
    for (auto id : due) {
      const auto found = records_.find(id);
      if (found == records_.end() || found->second.deadline > now) continue;
      ordered_.erase({found->second.deadline, id});
      auto executing = records_.extract(found);
      executing.mapped().expiryTask();
    }
  } catch (...) {
    running_ = false;
    throw;
  }
  running_ = false;
}

void TimerQueue::clearTimers() noexcept {
  assert(owner_ == std::this_thread::get_id());
  if (clearing_) return;
  clearing_ = true;
  {
    decltype(records_) cancelled;
    cancelled.swap(records_);
    ordered_.clear();
  }
  clearing_ = false;
}
}  // namespace hp::timer
