#include "base/ResourceBudget.h"

#include <algorithm>
#include <cassert>
#include <mutex>
#include <utility>

namespace hp::base {
struct ResourceBudgetState {
  explicit ResourceBudgetState(ResourceLimits configuredLimits) : limits(configuredLimits) {}
  std::mutex mutex;
  const ResourceLimits limits;
  ResourceSnapshot usage;
};

ResourceLease::ResourceLease(std::shared_ptr<ResourceBudgetState> state,
                             std::size_t connections,
                             std::size_t inputBytes,
                             std::size_t outputBytes) noexcept
    : state_(std::move(state)),
      connections_(connections),
      inputBytes_(inputBytes),
      outputBytes_(outputBytes) {}

ResourceLease::~ResourceLease() noexcept { reset(); }

ResourceLease::ResourceLease(ResourceLease&& other) noexcept
    : state_(std::move(other.state_)),
      connections_(std::exchange(other.connections_, 0)),
      inputBytes_(std::exchange(other.inputBytes_, 0)),
      outputBytes_(std::exchange(other.outputBytes_, 0)) {}

ResourceLease& ResourceLease::operator=(ResourceLease&& other) noexcept {
  if (this != &other) {
    reset();
    state_ = std::move(other.state_);
    connections_ = std::exchange(other.connections_, 0);
    inputBytes_ = std::exchange(other.inputBytes_, 0);
    outputBytes_ = std::exchange(other.outputBytes_, 0);
  }
  return *this;
}

void ResourceLease::reset() noexcept {
  if (!state_) return;
  {
    std::lock_guard lock(state_->mutex);
    state_->usage.connections.current -= connections_;
    state_->usage.inputBytes.current -= inputBytes_;
    state_->usage.outputBytes.current -= outputBytes_;
  }
  connections_ = inputBytes_ = outputBytes_ = 0;
  state_.reset();
}

void ResourceLease::releaseOutput(std::size_t bytes) noexcept {
  assert(bytes <= outputBytes_);
  if (!bytes) return;
  std::lock_guard lock(state_->mutex);
  outputBytes_ -= bytes;
  state_->usage.outputBytes.current -= bytes;
}

void ResourceLease::absorb(ResourceLease&& other) noexcept {
  if (!other.state_) return;
  assert(this != &other && (!state_ || state_ == other.state_));
  if (!state_) state_ = other.state_;
  // 同一账本已完成准入，总额不可能超出size_t上限；合并不再次计账。
  connections_ += std::exchange(other.connections_, 0);
  inputBytes_ += std::exchange(other.inputBytes_, 0);
  outputBytes_ += std::exchange(other.outputBytes_, 0);
  other.state_.reset();
}

bool ResourceLease::empty() const noexcept {
  return connections_ == 0 && inputBytes_ == 0 && outputBytes_ == 0;
}

ResourceBudget::ResourceBudget(ResourceLimits limits)
    : state_(std::make_shared<ResourceBudgetState>(limits)) {}

ResourceAcquisition ResourceBudget::tryAcquireConnection() {
  return tryAcquire(1, kTransportInputBytes, 0);
}

ResourceAcquisition ResourceBudget::tryAcquireInput(std::size_t bytes) {
  return tryAcquire(0, bytes, 0);
}

ResourceAcquisition ResourceBudget::tryAcquireOutput(std::size_t bytes) {
  return tryAcquire(0, 0, bytes);
}

ResourceAcquisition ResourceBudget::tryAcquire(std::size_t connections,
                                               std::size_t inputBytes,
                                               std::size_t outputBytes) {
  std::lock_guard lock(state_->mutex);
  auto& usage = state_->usage;
  const auto& limits = state_->limits;
  if (connections > limits.connections - usage.connections.current) {
    ++usage.connections.rejected;
    return {ResourceRejection::kConnections, {}};
  }
  if (inputBytes > limits.inputBytes - usage.inputBytes.current) {
    ++usage.inputBytes.rejected;
    return {ResourceRejection::kInputBytes, {}};
  }
  if (outputBytes > limits.outputBytes - usage.outputBytes.current) {
    ++usage.outputBytes.rejected;
    return {ResourceRejection::kOutputBytes, {}};
  }
  usage.connections.current += connections;
  usage.inputBytes.current += inputBytes;
  usage.outputBytes.current += outputBytes;
  usage.connections.peak = std::max(usage.connections.peak, usage.connections.current);
  usage.inputBytes.peak = std::max(usage.inputBytes.peak, usage.inputBytes.current);
  usage.outputBytes.peak = std::max(usage.outputBytes.peak, usage.outputBytes.current);
  return {ResourceRejection::kNone, ResourceLease{state_, connections, inputBytes, outputBytes}};
}

ResourceSnapshot ResourceBudget::snapshot() const {
  std::lock_guard lock(state_->mutex);
  return state_->usage;
}

std::string ResourceBudget::serializeSnapshot(const ResourceSnapshot& snapshot) {
  std::string result;
  for (const auto& [name, usage] : {std::pair{"connections", snapshot.connections},
                                    std::pair{"input_bytes", snapshot.inputBytes},
                                    std::pair{"output_bytes", snapshot.outputBytes}}) {
    const auto prefix = std::string("resource_") + name;
    result += prefix + "_current " + std::to_string(usage.current) + '\n';
    result += prefix + "_peak " + std::to_string(usage.peak) + '\n';
    result += prefix + "_rejected_total " + std::to_string(usage.rejected) + '\n';
  }
  return result;
}
}  // namespace hp::base
