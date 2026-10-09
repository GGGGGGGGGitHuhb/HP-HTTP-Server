#include "metrics/ServerMetrics.h"

#include <iostream>
#include <limits>
#include <thread>
#include <vector>

#include "TestCheck.h"

using hp::metrics::ServerMetrics;

void testSaturation() {
  std::atomic<std::uint64_t> count{std::numeric_limits<std::uint64_t>::max() - 1};
  ServerMetrics::addSaturated(count, 100);
  requireTestCondition(count == std::numeric_limits<std::uint64_t>::max());
  ServerMetrics::addSaturated(count, 1);
  requireTestCondition(count == std::numeric_limits<std::uint64_t>::max());
}

void countRequests(ServerMetrics& metrics) {
  for (int index = 0; index < 10000; ++index) {
    metrics.registerConnection();
    metrics.beginRequest();
    metrics.finishRequest(index % 2, index % 3 ? 200 : 404, 12);
    metrics.removeConnection();
  }
}

void testConcurrentCounts() {
  ServerMetrics metrics;
  std::vector<std::thread> threads;
  for (int index = 0; index < 4; ++index) threads.emplace_back(countRequests, std::ref(metrics));
  for (auto& thread : threads) thread.join();
  metrics.removeConnection();
  auto result = metrics.snapshot();
  requireTestCondition(result.requestsStarted == 40000 && result.responsesCompleted == 20000);
  requireTestCondition(result.requestsAborted == 20000 && result.latencyCount == 40000);
  requireTestCondition(result.latencySumUs == 480000 && result.latencyMaxUs == 12);
  requireTestCondition(result.connectionsTotal == 40000 && result.connectionsActive == 0);
  requireTestCondition(result.responseStatus[1] == 26664 && result.responseStatus[4] == 13336);
  requireTestCondition(result.errors == 26668);
  metrics.recordParseError();
  metrics.recordProviderError();
  metrics.recordAccessLogFailure();
  metrics.beginRequest();
  metrics.finishRequest(true, 0, 42);
  result = metrics.snapshot();
  requireTestCondition(result.responseStatus[0] == 1 && result.latencyMaxUs == 42);
  requireTestCondition(result.parseErrors == 1 && result.providerErrors == 1 &&
                       result.accessLogFailures == 1);
  const auto text = ServerMetrics::serializeSnapshot(result);
  requireTestCondition(text.find("responses_status_unknown_total 1\n") != std::string::npos);
  requireTestCondition(text.find("connections_active 0\n") != std::string::npos);
}

int main() {
  testSaturation();
  testConcurrentCounts();
  std::cout << "saturation/concurrent terminal/status/error/latency/connection/text: passed\n";
}
