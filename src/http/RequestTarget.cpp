#include "http/RequestTarget.h"

namespace hp::http {
namespace {
int hexDigit(unsigned char byte) {
  if (byte >= '0' && byte <= '9') return byte - '0';
  if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
  if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
  return -1;
}

bool decodeComponent(std::string_view raw, bool path, std::size_t limit, std::string& decoded) {
  for (std::size_t index = 0; index < raw.size(); ++index) {
    auto byte = static_cast<unsigned char>(raw[index]);
    if (byte == '%') {
      if (raw.size() - index < 3) return false;
      const int high = hexDigit(static_cast<unsigned char>(raw[index + 1]));
      const int low = hexDigit(static_cast<unsigned char>(raw[index + 2]));
      if (high < 0 || low < 0) return false;
      byte = static_cast<unsigned char>(high * 16 + low);
      index += 2;
      if (path && (byte == '/' || byte == '\\' || byte == '?' || byte == '#' || byte == '%'))
        return false;
    }
    if (byte < 0x20 || byte == 0x7f || decoded.size() == limit) return false;
    decoded.push_back(static_cast<char>(byte));
  }
  return true;
}

bool parseQuery(std::string_view raw, std::vector<QueryParameter>& parameters) {
  if (raw.size() > kMaxRawQueryBytes) return false;
  if (raw.empty()) return true;
  std::size_t start = 0, decodedBytes = 0;
  while (start <= raw.size()) {
    const auto separator = raw.find('&', start);
    const auto end = separator == std::string_view::npos ? raw.size() : separator;
    const auto pair = raw.substr(start, end - start);
    const auto equals = pair.find('=');
    const auto key = pair.substr(0, equals);
    const auto value =
        equals == std::string_view::npos ? std::string_view{} : pair.substr(equals + 1);
    if (key.empty() || parameters.size() == kMaxQueryParameters) return false;
    QueryParameter parameter;
    if (!decodeComponent(key, false, kMaxQueryKeyBytes, parameter.key) ||
        !decodeComponent(value, false, kMaxQueryValueBytes, parameter.value))
      return false;
    if (parameter.key.size() > kMaxDecodedQueryBytes - decodedBytes) return false;
    decodedBytes += parameter.key.size();
    if (parameter.value.size() > kMaxDecodedQueryBytes - decodedBytes) return false;
    decodedBytes += parameter.value.size();
    parameters.push_back(std::move(parameter));
    if (separator == std::string_view::npos) break;
    start = separator + 1;
  }
  return true;
}

bool splitPath(std::string_view path, std::vector<std::string>& components) {
  if (path.find('\\') != std::string_view::npos) return false;
  if (path == "/") {
    components.emplace_back("index.html");
    return true;
  }
  std::size_t start = 1;
  while (start <= path.size()) {
    const auto separator = path.find('/', start);
    const auto end = separator == std::string_view::npos ? path.size() : separator;
    const auto component = path.substr(start, end - start);
    if (component.empty() || component == "." || component == "..") return false;
    components.emplace_back(component);
    if (separator == std::string_view::npos) break;
    start = separator + 1;
  }
  return true;
}
}  // namespace

TargetParseResult parseRequestTarget(std::string_view rawTarget) {
  if (rawTarget.empty() || rawTarget.front() != '/' || rawTarget.size() > kMaxRawTargetBytes)
    return {Status::kBadRequest, {}};
  for (unsigned char byte : rawTarget)
    if (byte <= 0x20 || byte == 0x7f || byte == '#') return {Status::kBadRequest, {}};
  const auto separator = rawTarget.find('?');
  const auto rawPath = rawTarget.substr(0, separator);
  const auto rawQuery =
      separator == std::string_view::npos ? std::string_view{} : rawTarget.substr(separator + 1);
  std::string path;
  std::vector<QueryParameter> query;
  if (!decodeComponent(rawPath, true, kMaxRawTargetBytes, path) || !parseQuery(rawQuery, query))
    return {Status::kBadRequest, {}};
  std::vector<std::string> components;
  if (!splitPath(path, components)) return {Status::kForbidden, {}};
  return {Status::kOk, RequestTarget(std::move(path), std::move(components), std::move(query))};
}
}  // namespace hp::http
