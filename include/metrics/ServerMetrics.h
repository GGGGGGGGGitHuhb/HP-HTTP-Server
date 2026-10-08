#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace hp::metrics {

// runtime 快照逐字段采样；仅停止并销毁全部连接后的快照具终态一致性。
struct MetricsSnapshot {
  std::uint64_t requestsStarted{}, responsesCompleted{}, requestsAborted{};
  std::array<std::uint64_t, 7> responseStatus{};
  std::uint64_t errors{}, parseErrors{}, providerErrors{}, accessLogFailures{};
  std::uint64_t latencyCount{}, latencySumUs{}, latencyMaxUs{};
  std::uint64_t connectionsTotal{}, connectionsActive{};
};

class ServerMetrics final {
 public:
  void beginRequest() noexcept;
  void finishRequest(bool aborted, int status, std::uint64_t durationUs) noexcept;
  void recordParseError() noexcept;
  void recordProviderError() noexcept;
  void recordAccessLogFailure() noexcept;
  void registerConnection() noexcept;
  void removeConnection() noexcept;

  MetricsSnapshot snapshot() const noexcept;
  static std::string serializeSnapshot(const MetricsSnapshot& snapshot);
  // 所有增量采用饱和策略；用于计数及 sum，绝不回绕。
  static void addSaturated(std::atomic<std::uint64_t>& counter, std::uint64_t value) noexcept;

 private:
  std::atomic<std::uint64_t> requestsStarted_{0}, responsesCompleted_{0}, requestsAborted_{0};
  std::array<std::atomic<std::uint64_t>, 7> responseStatus_{};
  std::atomic<std::uint64_t> errors_{0}, parseErrors_{0}, providerErrors_{0}, accessLogFailures_{0};
  std::atomic<std::uint64_t> latencyCount_{0}, latencySumUs_{0}, latencyMaxUs_{0};
  std::atomic<std::uint64_t> connectionsTotal_{0}, connectionsActive_{0};
};
}  // namespace hp::metrics
