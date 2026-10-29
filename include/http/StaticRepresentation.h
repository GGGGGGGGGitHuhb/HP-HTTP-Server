#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "http/HttpRequest.h"

namespace hp::http {

struct StaticFileMetadata {
  std::uint64_t device, inode, size;
  std::int64_t modifiedSeconds, modifiedNanoseconds, changedSeconds, changedNanoseconds;
};

struct StaticResponseMetadata {
  std::string date;
  std::string entityTag;
  std::optional<std::string> lastModified;
  std::optional<std::string> contentRange;
};

enum class StaticSelection { kFull, kNotModified, kPartial, kUnsatisfiable };

struct ByteRangeSelection {
  StaticSelection selection{StaticSelection::kFull};
  std::uint64_t offset{0}, length{0};
};

struct StaticEvaluation {
  ByteRangeSelection range;
  StaticResponseMetadata metadata;
};

[[nodiscard]] std::optional<std::int64_t> parseHttpDate(std::string_view value,
                                                        std::int64_t nowSeconds);
[[nodiscard]] std::optional<std::string> formatHttpDate(std::int64_t seconds);
[[nodiscard]] ByteRangeSelection selectByteRange(std::string_view value, std::uint64_t size);
[[nodiscard]] StaticEvaluation evaluateStaticConditions(const StaticRequestFields& fields,
                                                        const StaticFileMetadata& file,
                                                        std::int64_t nowSeconds,
                                                        bool omitBody);

}  // namespace hp::http
