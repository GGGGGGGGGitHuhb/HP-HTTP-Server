#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "TestCheck.h"
#include "http/RequestRouter.h"
#include "metrics/ServerMetrics.h"

namespace {
using namespace hp::http;
constexpr std::int64_t kNow = 784111777;  // Sun, 06 Nov 1994 08:49:37 GMT

void checkRange(std::string_view wire,
                std::uint64_t size,
                StaticSelection selection,
                std::uint64_t offset,
                std::uint64_t length) {
  const auto result = selectByteRange(wire, size);
  requireTestCondition(result.selection == selection && result.offset == offset &&
                       result.length == length);
}

void testRanges() {
  const auto initial = selectByteRange("bytes=3-5", 10);
  requireTestCondition(initial.offset == (std::getenv("HP_STATIC_SEMANTICS_NEGATIVE") ? 4 : 3));
  for (auto wire : {"bytes=0-0", "BYTES=00-00", " \tbytes=0-0\t "})
    checkRange(wire, 10, StaticSelection::kPartial, 0, 1);
  checkRange("bytes=3-5", 10, StaticSelection::kPartial, 3, 3);
  checkRange("bytes=3-", 10, StaticSelection::kPartial, 3, 7);
  checkRange("bytes=3-18446744073709551615", 10, StaticSelection::kPartial, 3, 7);
  checkRange("bytes=-3", 10, StaticSelection::kPartial, 7, 3);
  checkRange("bytes=-99", 10, StaticSelection::kPartial, 0, 10);
  for (auto wire : {"bytes=-0", "bytes=10-", "bytes=18446744073709551615-"})
    checkRange(wire, 10, StaticSelection::kUnsatisfiable, 0, 0);
  for (auto wire : {"bytes=0-0", "bytes=-1", "bytes=0-"})
    checkRange(wire, 0, StaticSelection::kUnsatisfiable, 0, 0);
  for (auto wire : {"bytes=3-2",
                    "bytes=1-2,3-4",
                    "bytes=0-0,",
                    "units=0-1",
                    "bytes=+0-1",
                    "bytes=0 -1",
                    "bytes=0- 1",
                    "bytes=",
                    "bytes=-",
                    "bytes=--1",
                    "bytes=0-1x",
                    "bytes=18446744073709551616-",
                    "bytes=-18446744073709551616"})
    checkRange(wire, 10, StaticSelection::kFull, 0, 10);
  std::cout << "CASE S01-range-boundaries-overflow-negative PASS\n";
}

void testDates() {
  const std::string imf = "Sun, 06 Nov 1994 08:49:37 GMT";
  requireTestCondition(formatHttpDate(kNow) == imf);
  for (auto value : {imf,
                     std::string("Sunday, 06-Nov-94 08:49:37 GMT"),
                     std::string("Sun Nov  6 08:49:37 1994")})
    requireTestCondition(parseHttpDate(value, kNow) == kNow);
  for (auto value : {"Mon, 06 Nov 1994 08:49:37 GMT",
                     "Sun, 31 Nov 1994 08:49:37 GMT",
                     "Sun, 06 Nov 1994 24:49:37 GMT",
                     "Sun, 06 Nov 1994 08:60:37 GMT",
                     "Sun, 06 Nov 1994 08:49:60 GMT",
                     "Sun, 06 Nov 1994 08:49:37 UTC",
                     "Sun, 06 Nov 1994 08:49:37 GMTx",
                     " Sun, 06 Nov 1994 08:49:37 GMT",
                     "Tue, 29 Feb 1900 00:00:00 GMT",
                     "Sun Nov 00 08:49:37 1994"})
    requireTestCondition(!parseHttpDate(value, kNow));
  requireTestCondition(parseHttpDate("Tue, 29 Feb 2000 00:00:00 GMT", kNow).has_value());
  requireTestCondition(formatHttpDate(-1) == "Wed, 31 Dec 1969 23:59:59 GMT");
  requireTestCondition(!formatHttpDate(std::numeric_limits<std::int64_t>::min()));
  requireTestCondition(!formatHttpDate(std::numeric_limits<std::int64_t>::max()));
  using namespace std::chrono;
  const auto now = sys_days{year{2000} / January / 1}.time_since_epoch();
  const auto secondsNow = duration_cast<seconds>(now).count();
  const auto y2050 =
      duration_cast<seconds>(sys_days{year{2050} / January / 1}.time_since_epoch()).count();
  const auto y1951 =
      duration_cast<seconds>(sys_days{year{1951} / January / 1}.time_since_epoch()).count();
  requireTestCondition(parseHttpDate("Saturday, 01-Jan-50 00:00:00 GMT", secondsNow) == y2050);
  requireTestCondition(parseHttpDate("Monday, 01-Jan-51 00:00:00 GMT", secondsNow) == y1951);
  std::cout << "CASE S02-three-dates-calendar-weekday-century-UTC PASS\n";
}

void testRfc850TimestampBoundary() {
  struct DateCase {
    std::string_view wire;
    std::int64_t now;
    std::int64_t expected;
  };
  // 独立固定UTC时间戳：秒/月/日等界，跨年，闰日不归一到3月1日。
  const DateCase cases[]{
      {"Friday, 31-Dec-76 00:00:00 GMT", 1767225600, 220838400},
      {"Monday, 15-Jun-76 12:30:39 GMT", 1781526640, 3359449839},
      {"Monday, 15-Jun-76 12:30:40 GMT", 1781526640, 3359449840},
      {"Tuesday, 15-Jun-76 12:30:41 GMT", 1781526640, 203689841},
      {"Sunday, 14-Jun-76 23:59:59 GMT", 1781526640, 3359404799},
      {"Wednesday, 16-Jun-76 00:00:00 GMT", 1781526640, 203731200},
      {"Sunday, 31-May-76 23:59:59 GMT", 1781526640, 3358195199},
      {"Thursday, 01-Jul-76 00:00:00 GMT", 1781526640, 205027200},
      {"Thursday, 31-Dec-76 23:59:59 GMT", 1798761599, 3376684799},
      {"Saturday, 01-Jan-77 00:00:00 GMT", 1798761599, 220924800},
      {"Wednesday, 28-Feb-74 23:59:59 GMT", 1709208000, 3287087999},
      {"Friday, 01-Mar-74 00:00:00 GMT", 1709208000, 131328000},
      {"Sunday, 29-Feb-76 00:00:00 GMT", 1772323199, 194400000},
  };
  for (const auto& test : cases)
    requireTestCondition(parseHttpDate(test.wire, test.now) == test.expected);
  // 回退之后才校验最终日期与星期，不接受错误世纪的星期或无效日历。
  requireTestCondition(!parseHttpDate("Thursday, 31-Dec-76 00:00:00 GMT", 1767225600));
  requireTestCondition(!parseHttpDate("Sunday, 29-Feb-74 00:00:00 GMT", 1709208000));
  std::cout << "CASE S02b-RFC850-complete-timestamp-50-years PASS\n";
}

void testConditions() {
  StaticFileMetadata file{1, 2, 10, kNow - 1, 20, kNow - 2, 30};
  const auto baseline = evaluateStaticConditions({}, file, kNow, false);
  const auto tag = baseline.metadata.entityTag;
  requireTestCondition(tag == "W/\"1-2-a-2ebc98a0-14-2ebc989f-1e\"");
  requireTestCondition(evaluateStaticConditions({}, file, kNow, false).metadata.entityTag == tag);
  auto changed = file;
  ++changed.inode;
  requireTestCondition(evaluateStaticConditions({}, changed, kNow, false).metadata.entityTag !=
                       tag);
  for (auto match : {std::string("*"),
                     tag,
                     tag.substr(2),
                     std::string("\"other,comma\", ") + tag,
                     std::string("\"\", ") + tag}) {
    StaticRequestFields fields;
    fields.ifNoneMatch = {match, true, false};
    fields.range = {"bytes=100-", true, false};
    for (bool head : {false, true})
      requireTestCondition(evaluateStaticConditions(fields, file, kNow, head).range.selection ==
                           StaticSelection::kNotModified);
  }
  for (auto invalid : {std::string(""),
                       std::string("*, ") + tag,
                       tag + ",",
                       tag + ",garbage",
                       std::string("w/") + tag.substr(2),
                       std::string("\"bad space\", ") + tag,
                       std::string("\"OTHER\"")}) {
    StaticRequestFields fields;
    fields.ifNoneMatch = {invalid, true, false};
    fields.ifModifiedSince = {*formatHttpDate(kNow), true, false};
    requireTestCondition(evaluateStaticConditions(fields, file, kNow, false).range.selection ==
                         StaticSelection::kFull);
  }
  StaticRequestFields fields;
  std::string tags = tag;
  for (int i = 1; i < 128; ++i) tags += ",\"x\"";
  fields.ifNoneMatch = {tags, true, false};
  requireTestCondition(evaluateStaticConditions(fields, file, kNow, false).range.selection ==
                       StaticSelection::kNotModified);
  fields.ifNoneMatch.value += ",\"x\"";
  requireTestCondition(evaluateStaticConditions(fields, file, kNow, false).range.selection ==
                       StaticSelection::kFull);
  fields = {};
  fields.ifModifiedSince = {*formatHttpDate(kNow - 1), true, false};
  requireTestCondition(evaluateStaticConditions(fields, file, kNow, false).range.selection ==
                       StaticSelection::kNotModified);
  fields.ifModifiedSince.value = *formatHttpDate(kNow - 2);
  requireTestCondition(evaluateStaticConditions(fields, file, kNow, false).range.selection ==
                       StaticSelection::kFull);
  fields.ifModifiedSince = {*formatHttpDate(kNow), true, true};
  requireTestCondition(evaluateStaticConditions(fields, file, kNow, false).range.selection ==
                       StaticSelection::kFull);
  file.modifiedSeconds = kNow + 100;
  requireTestCondition(evaluateStaticConditions({}, file, kNow, false).metadata.lastModified ==
                       formatHttpDate(kNow));
  file.modifiedSeconds = std::numeric_limits<std::int64_t>::min();
  requireTestCondition(!evaluateStaticConditions(fields, file, kNow, false).metadata.lastModified);
  fields = {};
  fields.range = {"bytes=1-3", true, false};
  for (auto value :
       {tag, tag.substr(2), std::string(""), std::string("bad"), *formatHttpDate(kNow)}) {
    fields.ifRange = {value, true, false};
    requireTestCondition(evaluateStaticConditions(fields, file, kNow, false).range.selection ==
                         StaticSelection::kFull);
  }
  fields.ifRange = {};
  fields.range.duplicate = true;
  requireTestCondition(evaluateStaticConditions(fields, file, kNow, false).range.selection ==
                       StaticSelection::kFull);
  fields.range.duplicate = false;
  requireTestCondition(evaluateStaticConditions(fields, file, kNow, true).range.selection ==
                       StaticSelection::kFull);
  std::cout << "CASE S03-weak-tag-lists-128-129-IMS-priority-IfRange PASS\n";
}

void checkFields(const HttpRequest& request) {
  const auto& fields = request.staticFields;
  requireTestCondition(fields.ifNoneMatch.present && fields.ifNoneMatch.duplicate &&
                       fields.ifNoneMatch.value == "\"a,b\",W/\"c\"");
  requireTestCondition(fields.ifModifiedSince.duplicate && fields.range.duplicate &&
                       fields.ifRange.duplicate);
  requireTestCondition(fields.range.value == "bytes=1-2" && fields.ifRange.value.empty());
}

void testParser() {
  std::string wire =
      "GET / HTTP/1.1\r\nHost: x\r\nIf-None-Match: \t\"a,b\" \t\r\nif-none-match: W/\"c\"\r\n"
      "If-Modified-Since: bad\r\nIf-Modified-Since: other\r\nRange: bytes=1-2\r\nRange: bad\r\n"
      "If-Range:\r\nIf-Range: other\r\nContent-Length: 1\r\n\r\nx";
  for (std::size_t split = 1; split < wire.size(); ++split) {
    RequestParser parser;
    auto result = parser.feedRequestBytes(std::string_view(wire).substr(0, split));
    requireTestCondition(result.status == ParseStatus::kNeedMore &&
                         !result.request.staticFields.ifNoneMatch.present);
    result = parser.feedRequestBytes(std::string_view(wire).substr(split));
    requireTestCondition(result.status == ParseStatus::kComplete);
    checkFields(result.request);
    parser.resetRequestParser();
    const auto next = parser.feedRequestBytes("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    requireTestCondition(next.status == ParseStatus::kComplete &&
                         !next.request.staticFields.range.present);
    checkFields(result.request);
  }
  RequestParser parser;
  FeedResult result;
  for (char byte : wire) result = parser.feedRequestBytes(std::string_view(&byte, 1));
  wire.assign("overwritten");
  checkFields(result.request);
  const std::string prefix = "GET / HTTP/1.1\r\nHost: x\r\nIf-None-Match: ";
  const auto exact = prefix + std::string(kMaxHeaderBytes - prefix.size() - 4, 'a') + "\r\n\r\n";
  requireTestCondition(parseRequest(exact).status == ParseStatus::kComplete);
  requireTestCondition(
      parseRequest(prefix + std::string(kMaxHeaderBytes - prefix.size() - 3, 'a') + "\r\n\r\n")
          .status == ParseStatus::kBadRequest);
  requireTestCondition(
      parseRequest("GET / HTTP/1.1\r\nHost: x\r\nRange: nonsense\r\nContent-Length: +1\r\n\r\n")
          .status == ParseStatus::kBadRequest);
  requireTestCondition(parseRequest("GET / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: "
                                    "chunked\r\n\r\n0\r\nIf-None-Match: *\r\n\r\n")
                           .status == ParseStatus::kBadRequest);
  std::cout << "CASE S04-owned-fields-every-split-byte-reset-16KiB-framing-trailer PASS\n";
}

class RootFixture {
 public:
  RootFixture();
  ~RootFixture();
  std::filesystem::path directory;
  std::string body;
};

RootFixture::RootFixture() {
  const auto parent = std::filesystem::path(std::getenv("HP_S3_TEST_TMP_ROOT"));
  std::filesystem::create_directories(parent);
  auto name = (parent / "static-unit-XXXXXX").string();
  requireTestCondition(::mkdtemp(name.data()));
  directory = name;
  body.assign(1024 * 1024, '\0');
  for (std::size_t i = 0; i < body.size(); ++i) body[i] = static_cast<char>(i % 251);
  std::ofstream(directory / "data.bin", std::ios::binary).write(body.data(), body.size());
  std::ofstream(directory / "empty");
  std::ofstream(directory / "large").put('x');
  std::filesystem::resize_file(directory / "large", kMaxFileBytes + 1);
  std::filesystem::create_symlink("data.bin", directory / "link");
  const timespec times[2]{{kNow, 0}, {kNow, 123}};
  requireTestCondition(::utimensat(AT_FDCWD, (directory / "data.bin").c_str(), times, 0) == 0);
}

RootFixture::~RootFixture() {
  std::filesystem::remove_all(directory);
  std::cout << "FIXTURE removed " << directory << '\n';
}

std::string text(const std::vector<std::byte>& bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::size_t descriptorCount() {
  std::size_t count = 0;
  for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator("/proc/self/fd"))
    ++count;
  return count;
}

void testService(const RootFixture& fixture) {
  StaticFileService service(fixture.directory);
  RequestRouter router(service);
  HttpRequest request{"GET", "/data.bin", false, {}};
  request.staticFields.range = {"bytes=123-500000", true, false};
  const auto baseline = descriptorCount();
  {
    auto result = router.onResponse(request, ConnectionPolicy::kKeepAlive);
    requireTestCondition(result.status == Status::kPartialContent && result.file &&
                         result.file->offset() == 123 && result.file->remaining() == 499878 &&
                         result.contentBytes == 499878);
    requireTestCondition(text(result.bytes).find("Content-Range: bytes 123-500000/1048576\r\n") !=
                         std::string::npos);
  }
  const auto materialized = service.handleResponse(request, ConnectionPolicy::kKeepAlive);
  const auto materialText = text(materialized.bytes);
  requireTestCondition(!materialized.file &&
                       materialText.substr(materialText.find("\r\n\r\n") + 4) ==
                           fixture.body.substr(123, 499878));
  const auto built = text(service.buildResponseBytes(request));
  requireTestCondition(built.substr(built.find("\r\n\r\n") + 4) ==
                       fixture.body.substr(123, 499878));
  request.staticFields.ifNoneMatch = {"*", true, false};
  for (auto method : {"GET", "HEAD"}) {
    request.method = method;
    auto result = router.onResponse(request, ConnectionPolicy::kKeepAlive);
    requireTestCondition(result.status == Status::kNotModified && !result.file &&
                         result.contentBytes == 0);
    const auto header = text(result.bytes);
    requireTestCondition(header.ends_with("\r\n\r\n") &&
                         header.find("Content-Length:") == std::string::npos &&
                         header.find("Content-Type:") == std::string::npos);
  }
  request.staticFields.ifNoneMatch = {};
  auto head = router.onResponse(request, ConnectionPolicy::kKeepAlive);
  requireTestCondition(head.status == Status::kOk && !head.file && head.contentBytes == 0);
  request.method = "GET";
  request.staticFields.range.value = "bytes=1048576-";
  auto unsatisfied = router.onResponse(request, ConnectionPolicy::kKeepAlive);
  requireTestCondition(unsatisfied.status == Status::kRangeNotSatisfiable && !unsatisfied.file &&
                       unsatisfied.contentBytes == 26 &&
                       unsatisfied.effectivePolicy == ConnectionPolicy::kKeepAlive);
  request.staticFields.ifNoneMatch = {"*", true, false};
  for (auto path : {"/link", "/large", "/missing"}) {
    request.target = path;
    const auto error = router.onResponse(request, ConnectionPolicy::kClose);
    requireTestCondition(
        error.status ==
        (std::string_view(path) == "/missing" ? Status::kNotFound : Status::kForbidden));
  }
  request.method = "POST";
  request.target = "/api/echo";
  request.body = "echo";
  requireTestCondition(
      text(router.onResponse(request, ConnectionPolicy::kClose).bytes).ends_with("\r\n\r\necho"));
  requireTestCondition(descriptorCount() == baseline);
  std::cout << "CASE S05-fd-offset-pread-304-HEAD-416-error-priority-no-leak PASS\n";
}

void testMetrics() {
  hp::metrics::ServerMetrics metrics;
  for (int status : {200, 400, 403, 404, 405, 500, 413, 417, 501, 206, 304, 416}) {
    metrics.beginRequest();
    metrics.finishRequest(false, status, 1);
  }
  const auto snapshot = metrics.snapshot();
  requireTestCondition(snapshot.responseStatus.size() == 16 && snapshot.responseStatus[0] == 0);
  for (std::size_t i = 1; i < 13; ++i) requireTestCondition(snapshot.responseStatus[i] == 1);
  requireTestCondition(snapshot.responsesCompleted == 12 && snapshot.errors == 9 &&
                       snapshot.parseErrors == 0 && snapshot.providerErrors == 0);
  std::cout << "CASE S06-original-indices-plus-three-metrics PASS\n";
}
}  // namespace

int main() {
  testRanges();
  testDates();
  testRfc850TimestampBoundary();
  testConditions();
  testParser();
  {
    RootFixture fixture;
    testService(fixture);
  }
  testMetrics();
  std::cout << "Static semantics PASS\n";
}
