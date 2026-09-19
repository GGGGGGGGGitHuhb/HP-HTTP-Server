#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "http/http_request.h"
#include "http/http_response.h"

namespace {

int failures = 0;
int need_more_hits = 0;
int bad_request_hits = 0;
int method_not_allowed_hits = 0;

void Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

hp::http::ParseResult Parse(std::string_view request) {
  const auto result = hp::http::ParseRequest(request);
  if (result.status == hp::http::ParseStatus::kNeedMore) ++need_more_hits;
  if (result.status == hp::http::ParseStatus::kBadRequest) ++bad_request_hits;
  if (result.status == hp::http::ParseStatus::kMethodNotAllowed) {
    ++method_not_allowed_hits;
  }
  return result;
}

void ExpectBad(std::string_view request, std::string_view message) {
  Expect(Parse(request).status == hp::http::ParseStatus::kBadRequest, message);
}

std::string BytesToString(const std::vector<std::byte>& bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void TestFragmentedAndCompleteRequest() {
  Expect(Parse("GET / HTTP/1.1\r\nHost: ex").status ==
             hp::http::ParseStatus::kNeedMore,
         "partial header must need more bytes");

  constexpr std::string_view kComplete =
      "GET /asset.txt?x=1 HTTP/1.1\r\n"
      "Host: example\r\n"
      "X-Test: yes\r\n\r\n";
  const auto parsed = Parse(kComplete);
  Expect(parsed.status == hp::http::ParseStatus::kComplete,
         "valid GET must complete");
  Expect(parsed.request.method == "GET", "method must be retained");
  Expect(parsed.request.target == "/asset.txt?x=1",
         "origin target and query must be retained");
  Expect(parsed.consumed_bytes == kComplete.size(),
         "consumed bytes must end at the first header block");

  const std::string pipelined =
      std::string(kComplete) + "GET /second HTTP/1.1\r\nHost: x\r\n\r\n";
  const auto first = Parse(pipelined);
  Expect(first.status == hp::http::ParseStatus::kComplete &&
             first.consumed_bytes == kComplete.size(),
         "only the first request header block may be consumed");

  const std::string ignored_trailing =
      std::string(kComplete) + std::string("\n\0ignored", 9);
  const auto ignored = Parse(ignored_trailing);
  Expect(ignored.status == hp::http::ParseStatus::kComplete &&
             ignored.consumed_bytes == kComplete.size(),
         "bytes after the first header block must be ignored");
}

void TestMethodAndHostRules() {
  const auto post =
      Parse("POST / HTTP/1.1\r\nHost: example\r\nContent-Length: 0\r\n\r\n");
  Expect(post.status == hp::http::ParseStatus::kMethodNotAllowed,
         "syntactically valid non-GET method must be 405");
  Expect(post.request.method == "POST",
         "405 result must retain the parsed method");

  ExpectBad("GET / HTTP/1.1\r\nUser-Agent: test\r\n\r\n",
            "missing Host must be rejected");
  ExpectBad("GET / HTTP/1.1\r\nHost: a\r\nhOsT: b\r\n\r\n",
            "duplicate Host must be rejected case-insensitively");
  ExpectBad("GET / HTTP/1.1\r\nHost: \t \r\n\r\n",
            "empty Host after OWS trim must be rejected");
  ExpectBad("GET / HTTP/1.1\r\n Host: x\r\n\r\n",
            "obs-fold or leading whitespace must be rejected");
}

void TestRequestLineAndHeaderSyntax() {
  const std::vector<std::string> invalid = {
      "GET / HTTP/1.0\r\nHost: x\r\n\r\n",
      "GET http://example/ HTTP/1.1\r\nHost: x\r\n\r\n",
      "GET /#fragment HTTP/1.1\r\nHost: x\r\n\r\n",
      "GET  / HTTP/1.1\r\nHost: x\r\n\r\n",
      "GET / HTTP/1.1 extra\r\nHost: x\r\n\r\n",
      "G?T / HTTP/1.1\r\nHost: x\r\n\r\n",
      "GET / HTTP/1.1\nHost: x\n\n",
      "GET / HTTP/1.1\rXHost: x\r\n\r\n",
      "GET / HTTP/1.1\r\nBad Header: x\r\n\r\n",
      "GET / HTTP/1.1\r\nNoColon\r\n\r\n",
      "GET / HTTP/1.1\r\nX: value\x01\r\nHost: x\r\n\r\n",
  };
  for (const auto& request : invalid) {
    ExpectBad(request, "invalid request syntax must return 400");
  }

  std::string nul_request = "GET / HTTP/1.1\r\nHost: x";
  nul_request.push_back('\0');
  nul_request += "\r\n\r\n";
  ExpectBad(nul_request, "NUL in request bytes must return 400");
}

void TestLimits() {
  std::string long_line = "GET /";
  long_line.append(hp::http::kMaxRequestLineBytes, 'a');
  long_line += " HTTP/1.1\r\nHost: x\r\n\r\n";
  ExpectBad(long_line, "request line over 4 KiB must return 400");

  std::string at_request_limit(hp::http::kMaxRequestBytes, 'a');
  at_request_limit.replace(0, 4, "GET ");
  ExpectBad(at_request_limit,
            "16 KiB without a header terminator must return 400");

  std::string over_request_limit = "GET / HTTP/1.1\r\nHost: x\r\nX: ";
  over_request_limit.append(hp::http::kMaxRequestBytes, 'a');
  ExpectBad(over_request_limit, "request bytes beyond 16 KiB must return 400");

  std::string exact_line = "GET /";
  exact_line.append(hp::http::kMaxRequestLineBytes - 14, 'a');
  exact_line += " HTTP/1.1\r\nHost: x\r\n\r\n";
  Expect(exact_line.find("\r\n") == hp::http::kMaxRequestLineBytes,
         "exact request-line fixture must be 4 KiB");
  Expect(Parse(std::string_view(exact_line)
                   .substr(0, hp::http::kMaxRequestLineBytes))
                 .status == hp::http::ParseStatus::kNeedMore,
         "exact 4 KiB content prefix must await CRLF independent of chunking");
  Expect(Parse(exact_line).status == hp::http::ParseStatus::kComplete,
         "a request line exactly at 4 KiB must be accepted");

  std::string exact_request = "GET / HTTP/1.1\r\nHost: x\r\nX-Limit: ";
  exact_request.append(hp::http::kMaxRequestBytes - exact_request.size() - 4,
                       'a');
  exact_request += "\r\n\r\n";
  Expect(exact_request.size() == hp::http::kMaxRequestBytes,
         "exact request fixture must be 16 KiB");
  Expect(Parse(exact_request).status == hp::http::ParseStatus::kComplete,
         "a complete request exactly at 16 KiB must be accepted");
}

void TestResponseAndMime() {
  const std::vector<std::byte> body{std::byte{'A'},
                                    std::byte{0},
                                    std::byte{'B'}};
  const std::string response =
      BytesToString(hp::http::MakeResponse(hp::http::Status::kOk,
                                           body,
                                           "application/octet-stream"));
  Expect(response.starts_with("HTTP/1.1 200 OK\r\n"),
         "200 response must have the standard status line");
  Expect(response.find("Content-Length: 3\r\n") != std::string::npos,
         "binary body length must be exact");
  Expect(response.find("Connection: close\r\n\r\n") != std::string::npos,
         "response must use CRLF and close semantics");
  Expect(response.size() >= 3 &&
             response.substr(response.size() - 3) == std::string("A\0B", 3),
         "binary response body must preserve NUL bytes");

  const std::string method = BytesToString(
      hp::http::MakeErrorResponse(hp::http::Status::kMethodNotAllowed));
  Expect(method.find("Allow: GET\r\n") != std::string::npos,
         "405 must include Allow: GET");
  Expect(method.ends_with("405 Method Not Allowed\n"),
         "405 body must be deterministic");

  const std::string internal = BytesToString(
      hp::http::MakeErrorResponse(hp::http::Status::kInternalServerError));
  Expect(internal.starts_with("HTTP/1.1 500 Internal Server Error\r\n"),
         "500 response construction must be covered");

  for (auto policy : {hp::http::ConnectionPolicy::kClose,
                      hp::http::ConnectionPolicy::kKeepAlive}) {
    for (auto status : {hp::http::Status::kOk,
                        hp::http::Status::kBadRequest,
                        hp::http::Status::kForbidden,
                        hp::http::Status::kNotFound,
                        hp::http::Status::kMethodNotAllowed,
                        hp::http::Status::kInternalServerError}) {
      const auto raw =
          BytesToString(status == hp::http::Status::kOk
                            ? hp::http::MakeResponse(status,
                                                     body,
                                                     "application/octet-stream",
                                                     false,
                                                     policy)
                            : hp::http::MakeErrorResponse(status, policy));
      const auto boundary = raw.find("\r\n\r\n") + 4;
      const auto connection = raw.find("Connection: "),
                 length = raw.find("Content-Length: ");
      Expect(connection != std::string::npos &&
                 raw.find("Connection: ", connection + 1) == std::string::npos,
             "one Connection on every status");
      Expect(
          length != std::string::npos &&
              raw.find("Content-Length: ", length + 1) == std::string::npos &&
              std::stoull(raw.substr(length + 16)) == raw.size() - boundary,
          "one exact length on every status");
      Expect(raw.find(policy == hp::http::ConnectionPolicy::kClose
                          ? "Connection: close\r\n"
                          : "Connection: keep-alive\r\n") != std::string::npos,
             "explicit policy serialized for all statuses");
      if (status == hp::http::Status::kMethodNotAllowed)
        Expect(raw.find("Allow: GET\r\n") != std::string::npos,
               "405 policy preserves Allow");
    }
  }
  Expect(hp::http::ContentTypeForPath("x.html") == "text/html; charset=utf-8",
         "html MIME must be known");
  Expect(hp::http::ContentTypeForPath("x.CSS") == "text/css; charset=utf-8",
         "MIME lookup must be case-insensitive");
  Expect(hp::http::ContentTypeForPath("x.js") == "application/javascript",
         "JavaScript MIME must be known");
  Expect(hp::http::ContentTypeForPath("x.txt") == "text/plain; charset=utf-8",
         "text MIME must be known");
  Expect(hp::http::ContentTypeForPath("x.json") == "application/json",
         "JSON MIME must be known");
  Expect(hp::http::ContentTypeForPath("x.png") == "image/png",
         "PNG MIME must be known");
  Expect(hp::http::ContentTypeForPath("x.jpeg") == "image/jpeg",
         "JPEG MIME must be known");
  Expect(hp::http::ContentTypeForPath("x.gif") == "image/gif",
         "GIF MIME must be known");
  Expect(hp::http::ContentTypeForPath("x.svg") == "image/svg+xml",
         "SVG MIME must be known");
  Expect(
      hp::http::ContentTypeForPath("x.unknown") == "application/octet-stream",
      "unknown extension must use octet-stream");
}

}  // namespace

int main() {
  TestFragmentedAndCompleteRequest();
  TestMethodAndHostRules();
  TestRequestLineAndHeaderSyntax();
  TestLimits();
  TestResponseAndMime();

  if (failures != 0) {
    std::cerr << failures << " HTTP parser assertion(s) failed\n";
    return 1;
  }
  std::cout << "HTTP parser tests passed; need_more_hits=" << need_more_hits
            << " bad_request_hits=" << bad_request_hits
            << " method_not_allowed_hits=" << method_not_allowed_hits << '\n';
  return 0;
}
