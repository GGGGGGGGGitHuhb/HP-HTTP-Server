#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <thread>

#include "base/non_copyable.h"

namespace hp::timer {
struct TimerQueueTestAccess;

class TimerQueue final : private base::NonCopyable {
 public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;
  using Id = std::uint64_t;
  using TimerTask = std::function<void()>;

  TimerQueue() = default;
  ~TimerQueue() noexcept;

  Id Add(TimePoint deadline, TimerTask expiry_task);
  bool Reschedule(Id id, TimePoint deadline);
  bool Cancel(Id id);

  std::optional<TimePoint> next_deadline() const;
  std::size_t size() const;
  Id last_id() const;

  void RunDue(TimePoint now, Id cutoff = UINT64_MAX);
  void Clear() noexcept;

 private:
  friend struct TimerQueueTestAccess;

  static Id ExchangeNextIdForTest(Id value);
  void RequireOwner() const;

  struct Record {
    TimePoint deadline;
    TimerTask expiry_task;
  };

  const std::thread::id owner_{std::this_thread::get_id()};
  std::map<Id, Record> records_;
  std::set<std::pair<TimePoint, Id>> ordered_;
  Id last_id_{0};

  bool clearing_{false}, running_{false};
};
}  // namespace hp::timer
