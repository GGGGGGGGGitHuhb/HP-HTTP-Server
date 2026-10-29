#include "http/StaticRepresentation.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <limits>
#include <tuple>

namespace hp::http {
namespace {
using namespace std::chrono;
constexpr std::array<std::string_view, 7> kDays{"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
constexpr std::array<std::string_view, 7>
    kLongDays{"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
constexpr std::array<std::string_view, 12>
    kMonths{"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

std::string_view trimOws(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
  return value;
}

int decimal(std::string_view value) {
  int result = 0;
  if (value.empty()) return -1;
  for (char c : value) {
    if (c < '0' || c > '9') return -1;
    result = result * 10 + c - '0';
  }
  return result;
}

std::optional<year_month_day> calendarDate(std::int64_t value) {
  constexpr auto first = sys_days{year{0} / January / 1}.time_since_epoch();
  constexpr auto last = sys_days{year{9999} / December / 31}.time_since_epoch();
  const auto wholeDays = floor<days>(seconds{value});
  if (wholeDays < first || wholeDays > last) return std::nullopt;
  return year_month_day{sys_days{wholeDays}};
}

std::string paddedDecimal(unsigned value, std::size_t width) {
  auto text = std::to_string(value);
  return std::string(width - text.size(), '0') + text;
}

std::string hexadecimal(std::uint64_t value) {
  std::array<char, 16> bytes{};
  const auto result = std::to_chars(bytes.data(), bytes.data() + bytes.size(), value, 16);
  return {bytes.data(), result.ptr};
}

std::string makeEntityTag(const StaticFileMetadata& file) {
  return "W/\"" + hexadecimal(file.device) + '-' + hexadecimal(file.inode) + '-' +
         hexadecimal(file.size) + '-' +
         hexadecimal(static_cast<std::uint64_t>(file.modifiedSeconds)) + '-' +
         hexadecimal(static_cast<std::uint64_t>(file.modifiedNanoseconds)) + '-' +
         hexadecimal(static_cast<std::uint64_t>(file.changedSeconds)) + '-' +
         hexadecimal(static_cast<std::uint64_t>(file.changedNanoseconds)) + '"';
}

bool matchesEntityTag(std::string_view value, std::string_view tag) {
  value = trimOws(value);
  if (value == "*") return true;
  if (tag.starts_with("W/")) tag.remove_prefix(2);
  bool matched = false;
  unsigned count = 0;
  while (!value.empty()) {
    if (++count > 128) return false;
    if (value.starts_with("W/")) value.remove_prefix(2);
    if (value.empty() || value.front() != '"') return false;
    std::size_t end = 1;
    for (; end < value.size() && value[end] != '"'; ++end) {
      const auto byte = static_cast<unsigned char>(value[end]);
      if (byte < 0x21 || byte == 0x7f) return false;
    }
    if (end == value.size()) return false;
    matched = matched || value.substr(0, end + 1) == tag;
    value = trimOws(value.substr(end + 1));
    if (value.empty()) return matched;
    if (value.front() != ',') return false;
    value = trimOws(value.substr(1));
    if (value.empty()) return false;
  }
  return false;
}

bool parseUnsigned(std::string_view value, std::uint64_t& number) {
  if (value.empty()) return false;
  number = 0;
  for (char byte : value) {
    if (byte < '0' || byte > '9') return false;
    const auto digit = static_cast<std::uint64_t>(byte - '0');
    if (number > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return false;
    number = number * 10 + digit;
  }
  return true;
}
}  // namespace

std::optional<std::string> formatHttpDate(std::int64_t value) {
  const auto date = calendarDate(value);
  if (!date) return std::nullopt;
  const auto dayStart = sys_days{*date};
  const hh_mm_ss clock{sys_seconds{seconds{value}} - dayStart};
  return std::string(kDays[weekday{dayStart}.c_encoding()]) + ", " +
         paddedDecimal(unsigned(date->day()), 2) + ' ' +
         std::string(kMonths[unsigned(date->month()) - 1]) + ' ' +
         paddedDecimal(unsigned(int(date->year())), 4) + ' ' +
         paddedDecimal(clock.hours().count(), 2) + ':' + paddedDecimal(clock.minutes().count(), 2) +
         ':' + paddedDecimal(clock.seconds().count(), 2) + " GMT";
}

std::optional<std::int64_t> parseHttpDate(std::string_view value, std::int64_t nowSeconds) {
  std::string_view dayName, monthName, clock;
  int dateYear = -1, dateDay = -1;
  bool longDay = false;
  if (value.size() == 29 && value.substr(3, 2) == ", " && value[7] == ' ' && value[11] == ' ' &&
      value[16] == ' ' && value.substr(25) == " GMT") {
    dayName = value.substr(0, 3);
    dateDay = decimal(value.substr(5, 2));
    monthName = value.substr(8, 3);
    dateYear = decimal(value.substr(12, 4));
    clock = value.substr(17, 8);
  } else if (value.size() == 24 && value[3] == ' ' && value[7] == ' ' && value[10] == ' ' &&
             value[19] == ' ') {
    dayName = value.substr(0, 3);
    monthName = value.substr(4, 3);
    dateDay = decimal(value.substr(value[8] == ' ' ? 9 : 8, value[8] == ' ' ? 1 : 2));
    dateYear = decimal(value.substr(20, 4));
    clock = value.substr(11, 8);
  } else {
    const auto comma = value.find(',');
    if (comma == std::string_view::npos) return std::nullopt;
    dayName = value.substr(0, comma);
    longDay = true;
    const auto tail = value.substr(comma);
    if (tail.size() != 24 || tail.substr(0, 2) != ", " || tail[4] != '-' || tail[8] != '-' ||
        tail[11] != ' ' || tail.substr(20) != " GMT")
      return std::nullopt;
    dateDay = decimal(tail.substr(2, 2));
    monthName = tail.substr(5, 3);
    const int shortYear = decimal(tail.substr(9, 2));
    const auto current = calendarDate(nowSeconds);
    if (shortYear < 0 || !current) return std::nullopt;
    const int currentYear = int(current->year());
    dateYear = currentYear / 100 * 100 + shortYear;
    clock = tail.substr(12, 8);
  }
  const auto month = std::find(kMonths.begin(), kMonths.end(), monthName);
  if (dateYear < 0 || dateDay < 1 || month == kMonths.end() || clock.size() != 8 ||
      clock[2] != ':' || clock[5] != ':')
    return std::nullopt;
  const int hour = decimal(clock.substr(0, 2)), minute = decimal(clock.substr(3, 2)),
            second = decimal(clock.substr(6, 2));
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59)
    return std::nullopt;
  if (longDay) {
    const auto current = *calendarDate(nowSeconds);  // RFC850分支已验证now可表示。
    const auto currentDaySeconds =
        duration_cast<seconds>(sys_days{current}.time_since_epoch()).count();
    // 按完整UTC日历字段比较；闰日周年不归一化，最终候选才校验日历和星期。
    const auto candidate = std::tuple{dateYear,
                                      unsigned(month - kMonths.begin() + 1),
                                      unsigned(dateDay),
                                      std::int64_t{hour * 3600 + minute * 60 + second}};
    const auto threshold = std::tuple{int(current.year()) + 50,
                                      unsigned(current.month()),
                                      unsigned(current.day()),
                                      nowSeconds - currentDaySeconds};
    if (candidate > threshold) dateYear -= 100;
    if (dateYear < 0) return std::nullopt;
  }
  const year_month_day date{year{dateYear},
                            std::chrono::month{unsigned(month - kMonths.begin() + 1)},
                            day{unsigned(dateDay)}};
  if (!date.ok()) return std::nullopt;
  const auto dayStart = sys_days{date};
  const auto dayIndex = weekday{dayStart}.c_encoding();
  if (dayName != (longDay ? kLongDays[dayIndex] : kDays[dayIndex])) return std::nullopt;
  return duration_cast<seconds>(dayStart.time_since_epoch()).count() + hour * 3600 + minute * 60 +
         second;
}

ByteRangeSelection selectByteRange(std::string_view value, std::uint64_t size) {
  const ByteRangeSelection full{StaticSelection::kFull, 0, size};
  value = trimOws(value);
  if (value.size() < 6) return full;
  constexpr std::string_view unit = "bytes=";
  for (std::size_t i = 0; i < unit.size(); ++i) {
    char byte = value[i];
    if (byte >= 'A' && byte <= 'Z') byte += 'a' - 'A';
    if (byte != unit[i]) return full;
  }
  value.remove_prefix(6);
  const auto dash = value.find('-');
  if (dash == std::string_view::npos) return full;
  const auto firstText = value.substr(0, dash), lastText = value.substr(dash + 1);
  std::uint64_t first = 0, last = 0;
  if (firstText.empty()) {
    if (!parseUnsigned(lastText, last)) return full;
    if (size == 0 || last == 0) return {StaticSelection::kUnsatisfiable, 0, 0};
    const auto length = std::min(last, size);
    return {StaticSelection::kPartial, size - length, length};
  }
  if (!parseUnsigned(firstText, first) ||
      (!lastText.empty() && (!parseUnsigned(lastText, last) || last < first)))
    return full;
  if (size == 0 || first >= size) return {StaticSelection::kUnsatisfiable, 0, 0};
  last = lastText.empty() ? size - 1 : std::min(last, size - 1);
  return {StaticSelection::kPartial, first, last - first + 1};
}

StaticEvaluation evaluateStaticConditions(const StaticRequestFields& fields,
                                          const StaticFileMetadata& file,
                                          std::int64_t nowSeconds,
                                          bool omitBody) {
  StaticEvaluation result{{StaticSelection::kFull, 0, file.size},
                          {formatHttpDate(nowSeconds).value_or(""), makeEntityTag(file), {}, {}}};
  const auto modified = std::min(file.modifiedSeconds, nowSeconds);
  result.metadata.lastModified = formatHttpDate(modified);
  bool notModified = false;
  if (fields.ifNoneMatch.present)
    notModified = matchesEntityTag(fields.ifNoneMatch.value, result.metadata.entityTag);
  else if (fields.ifModifiedSince.present && !fields.ifModifiedSince.duplicate &&
           result.metadata.lastModified) {
    const auto since = parseHttpDate(fields.ifModifiedSince.value, nowSeconds);
    notModified = since && *since >= modified;
  }
  if (notModified)
    result.range = {StaticSelection::kNotModified, 0, 0};
  else if (!omitBody && fields.range.present && !fields.range.duplicate && !fields.ifRange.present)
    result.range = selectByteRange(fields.range.value, file.size);
  if (result.range.selection == StaticSelection::kPartial)
    result.metadata.contentRange = "bytes " + std::to_string(result.range.offset) + '-' +
                                   std::to_string(result.range.offset + result.range.length - 1) +
                                   '/' + std::to_string(file.size);
  else if (result.range.selection == StaticSelection::kUnsatisfiable)
    result.metadata.contentRange = "bytes */" + std::to_string(file.size);
  return result;
}
}  // namespace hp::http
