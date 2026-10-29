#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>

#include "TestCheck.h"
#include "http/RequestRouter.h"

namespace {
using hp::http::ConnectionPolicy;
using hp::http::parseRequestTarget;
using hp::http::Status;

void expectStatus(std::string_view raw, Status expected) {
  const auto parsed = parseRequestTarget(raw);
  requireTestCondition(parsed.status == expected &&
                       parsed.target.has_value() == (expected == Status::kOk));
}

void testDecodeAndOwnership() {
  static_assert(!std::is_default_constructible_v<hp::http::RequestTarget>);
  auto first = parseRequestTarget("/%61pi/echo?q=%2526");
  requireTestCondition(first.target.has_value());
  requireTestCondition(first.target->path() ==
                       (std::getenv("HP_REQUEST_TARGET_NEGATIVE") ? "/wrong" : "/api/echo"));
  requireTestCondition(first.target->queryParameters()[0].value == "%26");
  std::string raw =
      "/space%20name/%aF?q=%20&key=a%26b%3Dc%3Fd&key=%25&flag&a=&plus=+%2B&semi=a;b=c&path=../"
      "x\\y%23z";
  const auto original = raw;
  auto result = parseRequestTarget(raw);
  requireTestCondition(raw == original && result.status == Status::kOk && result.target);
  raw.assign("destroy original input");
  const auto& target = *result.target;
  requireTestCondition(target.path() == std::string("/space name/\xaf"));
  requireTestCondition(target.components() ==
                       std::vector<std::string>({"space name", std::string("\xaf")}));
  const auto& query = target.queryParameters();
  requireTestCondition(query.size() == 8);
  requireTestCondition(query[0].key == "q" && query[0].value == " ");
  requireTestCondition(query[1].key == "key" && query[1].value == "a&b=c?d");
  requireTestCondition(query[2].key == "key" && query[2].value == "%");
  requireTestCondition(query[3].key == "flag" && query[3].value.empty());
  requireTestCondition(query[4].key == "a" && query[4].value.empty());
  requireTestCondition(query[5].value == "++" && query[6].value == "a;b=c" &&
                       query[7].value == "../x\\y#z");
  auto next = parseRequestTarget("/?");
  requireTestCondition(next.target && next.target->queryParameters().empty() && query.size() == 8);
  requireTestCondition(next.target->components() == std::vector<std::string>{"index.html"});
  expectStatus("/a..b", Status::kOk);
  expectStatus("/x?q=a?b", Status::kOk);
  for (const auto rawTarget : {"",
                               "relative",
                               "http://x/",
                               "/x#f",
                               "/x?q=#",
                               "/a b",
                               "/a\t",
                               "/a\x7f",
                               "/%",
                               "/%1",
                               "/%G0",
                               "/%0g",
                               "/%00",
                               "/%1f",
                               "/%7F",
                               "/?q=%00",
                               "/?q=%1F",
                               "/?q=%7f"})
    expectStatus(rawTarget, Status::kBadRequest);
  std::cout << "CASE T01-one-decode-owned-raw-unchanged PASS\n";
}

void testPathSafety() {
  for (const auto path : {"/%2f",
                          "/%2F",
                          "/%5c",
                          "/%5C",
                          "/%3f",
                          "/%3F",
                          "/%23",
                          "/%25",
                          "/%252e%252e/x",
                          "/.%252E/x"})
    expectStatus(path, Status::kBadRequest);
  for (const auto path : {"/.",
                          "/..",
                          "/%2e",
                          "/%2E%2e/x",
                          "/.%2e/x",
                          "/a/./b",
                          "/a/../b",
                          "/a\\b",
                          "//x",
                          "/a//b",
                          "/a/"}) {
    expectStatus(path, Status::kForbidden);
    expectStatus(std::string(path) + "?q=%2G", Status::kBadRequest);
  }
  expectStatus("/a..b?q=../../outside", Status::kOk);
  std::cout << "CASE T02-path-security-and-400-before-403 PASS\n";
}

void testQueryLimits() {
  expectStatus("/" + std::string(4095, 'a'), Status::kOk);
  expectStatus("/" + std::string(4096, 'a'), Status::kBadRequest);
  expectStatus("/?" + std::string(128, 'k'), Status::kOk);
  expectStatus("/?" + std::string(129, 'k'), Status::kBadRequest);
  expectStatus("/?k=" + std::string(1024, 'v'), Status::kOk);
  expectStatus("/?k=" + std::string(1025, 'v'), Status::kBadRequest);
  std::string encodedKey;
  for (int i = 0; i < 128; ++i) encodedKey += "%61";
  expectStatus("/?" + encodedKey, Status::kOk);
  expectStatus("/?" + encodedKey + "%61", Status::kBadRequest);
  std::string pairs;
  for (int i = 0; i < 64; ++i) pairs += (i ? "&k" : "k");
  const auto result = parseRequestTarget("/?" + pairs);
  requireTestCondition(result.target && result.target->queryParameters().size() == 64);
  expectStatus("/?" + pairs + "&k", Status::kBadRequest);
  const auto query = "a=" + std::string(1024, 'x') + "&b=" + std::string(1019, 'y');
  requireTestCondition(query.size() == 2048);
  expectStatus("/?" + query, Status::kOk);
  expectStatus("/?" + query + "y", Status::kBadRequest);
  const auto maximum = parseRequestTarget("/?" + query);
  std::size_t decodedBytes = 0;
  for (const auto& parameter : maximum.target->queryParameters())
    decodedBytes += parameter.key.size() + parameter.value.size();
  requireTestCondition(decodedBytes == 2045 && decodedBytes <= hp::http::kMaxDecodedQueryBytes);
  // 解码不扩张且&/=占raw预算，聚合2048上界被raw2048隐含限制，不能构造合法raw使聚合超界。
  for (const auto invalid : {"/?=x", "/?&k=v", "/?k=v&", "/?k&&v", "/?a=%", "/?a=%zz"})
    expectStatus(invalid, Status::kBadRequest);
  std::cout << "CASE T03-query-count-key-value-raw-and-aggregate-bounds PASS\n";
}

class RootFixture {
 public:
  RootFixture();
  ~RootFixture();

  std::filesystem::path directory;
  std::filesystem::path root;
};

RootFixture::RootFixture() {
  const auto parent = std::filesystem::path(std::getenv("HP_S3_TEST_TMP_ROOT"));
  std::filesystem::create_directories(parent);
  auto name = (parent / "target-unit-XXXXXX").string();
  requireTestCondition(::mkdtemp(name.data()));
  directory = name;
  root = directory / "www";
  std::filesystem::create_directories(root / "api");
  std::ofstream(root / "index.html") << "home\n";
  std::ofstream(root / "space name.txt") << "space\n";
  std::ofstream(root / std::string("high\xff.bin")) << "high\n";
  std::ofstream(root / "a..b") << "dots\n";
  std::ofstream(root / "api/echo") << "shadow-must-not-leak";
  std::ofstream(directory / "secret") << "outside-secret-must-not-leak";
  std::filesystem::create_symlink(directory / "secret", root / "link");
  std::filesystem::create_directory_symlink(directory, root / "dirlink");
}

RootFixture::~RootFixture() {
  std::filesystem::remove_all(directory);
  std::cout << "FIXTURE removed " << directory << '\n';
}

std::string text(const std::vector<std::byte>& bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string withoutDynamicDate(const std::vector<std::byte>& bytes) {
  auto response = text(bytes);
  const auto boundary = response.find("\r\n\r\n");
  requireTestCondition(boundary != std::string::npos);
  std::size_t count = 0, position = 0;
  while ((position = response.find("\r\nDate: ", position)) != std::string::npos &&
         position < boundary) {
    const auto end = response.find("\r\n", position + 2);
    const auto date = response.substr(position + 8, end - position - 8);
    const auto parsed = hp::http::parseHttpDate(date, 0);
    requireTestCondition(parsed && hp::http::formatHttpDate(*parsed) == date);
    ++count;
    position = end;
  }
  if (response.starts_with("HTTP/1.1 200 "))
    requireTestCondition(count == 1);
  else
    requireTestCondition(count == 0);
  const auto dateStart = response.find("\r\nDate: ");
  if (count) response.erase(dateStart + 8, response.find("\r\n", dateStart + 2) - dateStart - 8);
  return response;
}

void testIntegration(const RootFixture& fixture) {
  hp::http::StaticFileService service(fixture.root.string());
  hp::http::RequestRouter router(service);
  for (const auto path : {"/",
                          "/space%20name.txt?q=../secret",
                          "/high%FF.bin",
                          "/a..b",
                          "/missing",
                          "/l%69nk",
                          "/dirlink/secret",
                          "/%2e%2e/secret",
                          "/bad%2G",
                          "/../secret?q=%GG"}) {
    for (const auto method : {"GET", "HEAD"}) {
      hp::http::HttpRequest request{method, path, false, {}};
      const auto before = request.target;
      auto direct = service.onResponse(request, ConnectionPolicy::kKeepAlive);
      auto routed = router.onResponse(request, ConnectionPolicy::kKeepAlive);
      requireTestCondition(request.target == before && direct.status == routed.status &&
                           direct.effectivePolicy == routed.effectivePolicy &&
                           withoutDynamicDate(direct.bytes) == withoutDynamicDate(routed.bytes));
      const auto parsed = parseRequestTarget(path);
      if (parsed.target) {
        auto validated = service.onValidatedTarget(*parsed.target,
                                                   std::string_view(method) == "HEAD",
                                                   ConnectionPolicy::kKeepAlive);
        requireTestCondition(withoutDynamicDate(validated.bytes) ==
                                 withoutDynamicDate(routed.bytes) &&
                             validated.status == routed.status);
      } else
        requireTestCondition(routed.effectivePolicy == ConnectionPolicy::kClose);
      const auto full = service.buildResponseBytes(request, ConnectionPolicy::kKeepAlive);
      requireTestCondition(text(full).find("outside-secret") == std::string::npos);
      if (std::string_view(method) == "HEAD")
        requireTestCondition(!routed.file && routed.contentBytes == 0 &&
                             text(full).ends_with("\r\n\r\n"));
    }
  }
  for (const auto method : {"POST", "GET", "HEAD", "FROB"}) {
    const auto result = router.onResponse({method, "/%61pi/echo?q=%2526", false, "body"},
                                          ConnectionPolicy::kKeepAlive);
    requireTestCondition(
        result.status ==
        (std::string_view(method) == "POST" ? Status::kOk : Status::kMethodNotAllowed));
    if (result.status == Status::kOk)
      requireTestCondition(text(result.bytes).ends_with("\r\n\r\nbody"));
    else
      requireTestCondition(text(result.bytes).find("Allow: POST\r\n") != std::string::npos);
    const auto admission = router.onRequestHeaders({method, "/%61pi/echo?q=%2526", false},
                                                   ConnectionPolicy::kKeepAlive);
    requireTestCondition(admission.has_value() == (std::string_view(method) != "POST"));
    auto bad =
        router.onResponse({method, "/../x?bad=%G0", false, {}}, ConnectionPolicy::kKeepAlive);
    requireTestCondition(bad.status == Status::kBadRequest &&
                         bad.effectivePolicy == ConnectionPolicy::kClose);
  }
  std::cout << "CASE T04-static-router-validated-object-HEAD-security PASS\n";
}
}  // namespace

int main() {
  testDecodeAndOwnership();
  testPathSafety();
  testQueryLimits();
  RootFixture fixture;
  testIntegration(fixture);
  std::cout << "Request target baseline PASS\n";
}
