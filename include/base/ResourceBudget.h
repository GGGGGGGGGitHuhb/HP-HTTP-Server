#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>

namespace hp::base {
inline constexpr std::size_t kTransportInputBytes = 16U * 1024U;
inline constexpr std::size_t kRequestInputBytes = 1040U * 1024U;

struct ResourceLimits {
  std::size_t connections{128};
  std::size_t inputBytes{64U * 1024U * 1024U};
  std::size_t outputBytes{64U * 1024U * 1024U};
};

struct ResourceUsage {
  std::size_t current{0}, peak{0};
  std::uint64_t rejected{0};
};

struct ResourceSnapshot {
  ResourceUsage connections, inputBytes, outputBytes;
};

enum class ResourceRejection { kNone, kConnections, kInputBytes, kOutputBytes };

class ResourceLimitError final : public std::exception {
 public:
  const char* what() const noexcept override { return "shared resource limit exceeded"; }
};

struct ResourceBudgetState;

class ResourceLease final {
 public:
  ResourceLease() noexcept = default;
  ~ResourceLease() noexcept;
  ResourceLease(ResourceLease&& other) noexcept;
  ResourceLease& operator=(ResourceLease&& other) noexcept;
  ResourceLease(const ResourceLease&) = delete;
  ResourceLease& operator=(const ResourceLease&) = delete;

  void reset() noexcept;
  void releaseOutput(std::size_t bytes) noexcept;
  void absorb(ResourceLease&& other) noexcept;
  [[nodiscard]] bool empty() const noexcept;

 private:
  friend class ResourceBudget;
  ResourceLease(std::shared_ptr<ResourceBudgetState> state,
                std::size_t connections,
                std::size_t inputBytes,
                std::size_t outputBytes) noexcept;

  std::shared_ptr<ResourceBudgetState> state_;
  std::size_t connections_{0}, inputBytes_{0}, outputBytes_{0};
};

struct ResourceAcquisition {
  ResourceRejection rejection{ResourceRejection::kNone};
  ResourceLease lease;
  [[nodiscard]] bool accepted() const noexcept { return rejection == ResourceRejection::kNone; }
};

class ResourceBudget final {
 public:
  explicit ResourceBudget(ResourceLimits limits = {});
  [[nodiscard]] ResourceAcquisition tryAcquireConnection();
  [[nodiscard]] ResourceAcquisition tryAcquireInput(std::size_t bytes);
  [[nodiscard]] ResourceAcquisition tryAcquireOutput(std::size_t bytes);
  [[nodiscard]] ResourceSnapshot snapshot() const;
  [[nodiscard]] static std::string serializeSnapshot(const ResourceSnapshot& snapshot);

 private:
  ResourceAcquisition tryAcquire(std::size_t connections,
                                 std::size_t inputBytes,
                                 std::size_t outputBytes);
  std::shared_ptr<ResourceBudgetState> state_;
};
}  // namespace hp::base
