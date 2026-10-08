#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "metrics/ServerMetrics.h"

namespace hp::app {
struct AccessRecord {
  static constexpr std::size_t kMethodLimit = 16, kPathLimit = 96;
  std::array<char, kMethodLimit> method{};
  std::array<char, kPathLimit> path{};
  std::size_t methodLength{}, pathLength{};
  int status{};
  std::uint64_t contentBytes{}, durationUs{};
  bool aborted{}, pathTruncated{};

  void copyRequest(std::string_view requestMethod, std::string_view target) noexcept;
};

std::string serializeAccessRecord(const AccessRecord& record);
void submitAccessRecord(const AccessRecord& record, metrics::ServerMetrics* metrics) noexcept;
}  // namespace hp::app
