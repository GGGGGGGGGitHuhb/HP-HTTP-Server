#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <thread>

#include "base/NonCopyable.h"

namespace hp::timer {
struct TimerQueueTestAccess;

class TimerQueue final : private base::NonCopyable {
 public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;
  using Id = std::uint64_t;
  using TimerCallback = std::function<void()>;

  TimerQueue() = default;
  ~TimerQueue() noexcept;

  Id add(TimePoint deadline, TimerCallback expiryTask);
  bool reschedule(Id id, TimePoint deadline);
  bool cancel(Id id);

  std::optional<TimePoint> nextDeadline() const;
  std::size_t size() const;
  Id lastId() const;

  void runDue(TimePoint now, Id cutoff = UINT64_MAX);
  void clear() noexcept;

 private:
  friend struct TimerQueueTestAccess;

  static Id exchangeNextIdForTest(Id value);
  void requireOwner() const;

  struct Record {
    TimePoint deadline;
    TimerCallback expiryTask;
  };

  const std::thread::id owner_{std::this_thread::get_id()};
  std::map<Id, Record> records_;
  std::set<std::pair<TimePoint, Id>> ordered_;
  Id lastId_{0};

  bool clearing_{false}, running_{false};
};
}  // namespace hp::timer
