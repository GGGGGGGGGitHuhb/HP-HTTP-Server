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
  using Task = std::function<void()>;

  TimerQueue() = default;
  ~TimerQueue() noexcept;

  Id add(TimePoint deadline, Task callback);
  bool reschedule(Id id, TimePoint deadline);
  bool cancel(Id id);

  std::optional<TimePoint> next_deadline() const;
  std::size_t size() const;
  Id last_id() const;

  void run_due(TimePoint now, Id cutoff = UINT64_MAX);
  void clear() noexcept;

 private:
  friend struct TimerQueueTestAccess;

  static Id exchange_next_id_for_test(Id value);
  void require_owner() const;

  struct Record {
    TimePoint deadline;
    Task callback;
  };

  const std::thread::id owner_{std::this_thread::get_id()};
  std::map<Id, Record> records_;
  std::set<std::pair<TimePoint, Id>> ordered_;
  Id last_id_{0};

  bool clearing_{false}, running_{false};
};
}  // namespace hp::timer
