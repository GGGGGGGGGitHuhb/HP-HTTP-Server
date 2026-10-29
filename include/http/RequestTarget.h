#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "http/HttpResponse.h"

namespace hp::http {
inline constexpr std::size_t kMaxRawTargetBytes = 4096;
inline constexpr std::size_t kMaxRawQueryBytes = 2048;
inline constexpr std::size_t kMaxQueryParameters = 64;
inline constexpr std::size_t kMaxQueryKeyBytes = 128;
inline constexpr std::size_t kMaxQueryValueBytes = 1024;
inline constexpr std::size_t kMaxDecodedQueryBytes = 2048;

struct QueryParameter {
  std::string key;
  std::string value;
};

struct TargetParseResult;
[[nodiscard]] TargetParseResult parseRequestTarget(std::string_view rawTarget);

class RequestTarget final {
 public:
  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] const std::vector<std::string>& components() const noexcept { return components_; }
  [[nodiscard]] const std::vector<QueryParameter>& queryParameters() const noexcept {
    return query_;
  }

 private:
  friend TargetParseResult parseRequestTarget(std::string_view rawTarget);
  RequestTarget(std::string path,
                std::vector<std::string> components,
                std::vector<QueryParameter> query)
      : path_(std::move(path)), components_(std::move(components)), query_(std::move(query)) {}

  std::string path_;
  std::vector<std::string> components_;
  std::vector<QueryParameter> query_;
};

struct TargetParseResult {
  Status status{Status::kBadRequest};
  std::optional<RequestTarget> target{};
};
}  // namespace hp::http
