#include "timer/timer_queue.h"

#include <atomic>
#include <cassert>
#include <limits>
#include <stdexcept>
#include <vector>

namespace hp::timer {
namespace {
std::atomic<TimerQueue::Id> next_id{1};

TimerQueue::Id AllocateId() {
  auto id = next_id.load(std::memory_order_relaxed);
  do {
    if (!id) throw std::overflow_error("timer id exhausted");
  } while (!next_id.compare_exchange_weak(id,
                                          id == UINT64_MAX ? 0 : id + 1,
                                          std::memory_order_relaxed));
  return id;
}
}  // namespace

TimerQueue::Id TimerQueue::ExchangeNextIdForTest(Id value) {
  return next_id.exchange(value);
}

void TimerQueue::RequireOwner() const {
  if (owner_ != std::this_thread::get_id())
    throw std::logic_error("TimerQueue wrong owner");
}

TimerQueue::~TimerQueue() noexcept {
  assert(owner_ == std::this_thread::get_id());
  Clear();
}

TimerQueue::Id TimerQueue::Add(TimePoint deadline, TimerTask expiry_task) {
  RequireOwner();
  if (clearing_) throw std::logic_error("TimerQueue is clearing");
  if (!expiry_task) throw std::invalid_argument("empty timer callback");
  const auto id = AllocateId();
  auto [position, inserted] =
      records_.emplace(id, Record{deadline, std::move(expiry_task)});
  (void)inserted;
  try {
    ordered_.emplace(deadline, id);
  } catch (...) {
    auto rejected = records_.extract(position);
    throw;
  }
  last_id_ = id;
  return id;
}

bool TimerQueue::Reschedule(Id id, TimePoint deadline) {
  RequireOwner();
  const auto found = records_.find(id);
  if (found == records_.end()) return false;
  if (found->second.deadline == deadline) return true;
  ordered_.emplace(deadline,
                   id);  // Allocation failure preserves both old indices.
  ordered_.erase({found->second.deadline, id});
  found->second.deadline = deadline;
  return true;
}

bool TimerQueue::Cancel(Id id) {
  RequireOwner();
  const auto found = records_.find(id);
  if (found == records_.end()) return false;
  ordered_.erase({found->second.deadline, id});
  auto cancelled = records_.extract(
      found);  // Destroy captures only after both indices agree.
  return true;
}

std::optional<TimerQueue::TimePoint> TimerQueue::next_deadline() const {
  RequireOwner();
  if (ordered_.empty()) return {};
  return ordered_.begin()->first;
}

std::size_t TimerQueue::size() const {
  RequireOwner();
  return records_.size();
}

TimerQueue::Id TimerQueue::last_id() const {
  RequireOwner();
  return last_id_;
}

void TimerQueue::RunDue(TimePoint now, Id cutoff) {
  RequireOwner();
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
      executing.mapped().expiry_task();
    }
  } catch (...) {
    running_ = false;
    throw;
  }
  running_ = false;
}

void TimerQueue::Clear() noexcept {
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
