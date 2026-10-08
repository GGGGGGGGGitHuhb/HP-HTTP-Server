#include "metrics/ServerMetrics.h"

#include <limits>
#include <sstream>

namespace hp::metrics {
namespace {
constexpr std::array<int, 7> kStatusCodes{0, 200, 400, 403, 404, 405, 500};
}

void ServerMetrics::addSaturated(std::atomic<std::uint64_t>& counter,
                                 std::uint64_t value) noexcept {
  auto previous = counter.load(std::memory_order_relaxed);
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  while (!counter.compare_exchange_weak(previous,
                                        value > maximum - previous ? maximum : previous + value,
                                        std::memory_order_relaxed)) {
  }
}

void ServerMetrics::beginRequest() noexcept { addSaturated(requestsStarted_, 1); }

void ServerMetrics::finishRequest(bool aborted, int status, std::uint64_t durationUs) noexcept {
  addSaturated(aborted ? requestsAborted_ : responsesCompleted_, 1);
  std::size_t bucket = 0;
  for (std::size_t index = 1; index < kStatusCodes.size(); ++index)
    if (status == kStatusCodes[index]) bucket = index;
  addSaturated(responseStatus_[bucket], 1);
  if (aborted || status >= 400) addSaturated(errors_, 1);
  addSaturated(latencyCount_, 1);
  addSaturated(latencySumUs_, durationUs);
  auto previous = latencyMaxUs_.load(std::memory_order_relaxed);
  while (previous < durationUs &&
         !latencyMaxUs_.compare_exchange_weak(previous, durationUs, std::memory_order_relaxed)) {
  }
}

void ServerMetrics::recordParseError() noexcept { addSaturated(parseErrors_, 1); }

void ServerMetrics::recordProviderError() noexcept { addSaturated(providerErrors_, 1); }

void ServerMetrics::recordAccessLogFailure() noexcept { addSaturated(accessLogFailures_, 1); }

void ServerMetrics::registerConnection() noexcept {
  addSaturated(connectionsTotal_, 1);
  addSaturated(connectionsActive_, 1);
}

void ServerMetrics::removeConnection() noexcept {
  auto previous = connectionsActive_.load(std::memory_order_relaxed);
  while (previous && !connectionsActive_.compare_exchange_weak(previous,
                                                               previous - 1,
                                                               std::memory_order_relaxed)) {
  }
}

MetricsSnapshot ServerMetrics::snapshot() const noexcept {
  MetricsSnapshot result;
  result.requestsStarted = requestsStarted_.load(std::memory_order_relaxed);
  result.responsesCompleted = responsesCompleted_.load(std::memory_order_relaxed);
  result.requestsAborted = requestsAborted_.load(std::memory_order_relaxed);
  for (std::size_t index = 0; index < responseStatus_.size(); ++index)
    result.responseStatus[index] = responseStatus_[index].load(std::memory_order_relaxed);
  result.errors = errors_.load(std::memory_order_relaxed);
  result.parseErrors = parseErrors_.load(std::memory_order_relaxed);
  result.providerErrors = providerErrors_.load(std::memory_order_relaxed);
  result.accessLogFailures = accessLogFailures_.load(std::memory_order_relaxed);
  result.latencyCount = latencyCount_.load(std::memory_order_relaxed);
  result.latencySumUs = latencySumUs_.load(std::memory_order_relaxed);
  result.latencyMaxUs = latencyMaxUs_.load(std::memory_order_relaxed);
  result.connectionsTotal = connectionsTotal_.load(std::memory_order_relaxed);
  result.connectionsActive = connectionsActive_.load(std::memory_order_relaxed);
  return result;
}

std::string ServerMetrics::serializeSnapshot(const MetricsSnapshot& snapshot) {
  std::ostringstream output;
  output << "requests_started_total " << snapshot.requestsStarted << '\n'
         << "responses_completed_total " << snapshot.responsesCompleted << '\n'
         << "requests_aborted_total " << snapshot.requestsAborted << '\n';
  for (std::size_t index = 0; index < kStatusCodes.size(); ++index)
    output << "responses_status_" << (index ? std::to_string(kStatusCodes[index]) : "unknown")
           << "_total " << snapshot.responseStatus[index] << '\n';
  output << "errors_total " << snapshot.errors << '\n'
         << "parse_errors_total " << snapshot.parseErrors << '\n'
         << "provider_errors_total " << snapshot.providerErrors << '\n'
         << "access_log_failures_total " << snapshot.accessLogFailures << '\n'
         << "latency_count " << snapshot.latencyCount << '\n'
         << "latency_sum_us " << snapshot.latencySumUs << '\n'
         << "latency_max_us " << snapshot.latencyMaxUs << '\n'
         << "connections_total " << snapshot.connectionsTotal << '\n'
         << "connections_active " << snapshot.connectionsActive << '\n';
  return output.str();
}
}  // namespace hp::metrics
