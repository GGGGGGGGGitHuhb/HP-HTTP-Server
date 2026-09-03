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

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

hp::http::ParseResult parse(std::string_view request) {
    const auto result = hp::http::parse_request(request);
    if (result.status == hp::http::ParseStatus::need_more) ++need_more_hits;
    if (result.status == hp::http::ParseStatus::bad_request) ++bad_request_hits;
    if (result.status == hp::http::ParseStatus::method_not_allowed) {
        ++method_not_allowed_hits;
    }
    return result;
}

void expect_bad(std::string_view request, std::string_view message) {
    expect(parse(request).status == hp::http::ParseStatus::bad_request,
           message);
}

std::string bytes_to_string(const std::vector<std::byte>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void test_fragmented_and_complete_request() {
    expect(parse("GET / HTTP/1.1\r\nHost: ex").status ==
               hp::http::ParseStatus::need_more,
           "partial header must need more bytes");

    constexpr std::string_view complete =
        "GET /asset.txt?x=1 HTTP/1.1\r\n"
        "Host: example\r\n"
        "X-Test: yes\r\n\r\n";
    const auto parsed = parse(complete);
    expect(parsed.status == hp::http::ParseStatus::complete,
           "valid GET must complete");
    expect(parsed.request.method == "GET", "method must be retained");
    expect(parsed.request.target == "/asset.txt?x=1",
           "origin target and query must be retained");
    expect(parsed.consumed_bytes == complete.size(),
           "consumed bytes must end at the first header block");

    const std::string pipelined =
        std::string(complete) + "GET /second HTTP/1.1\r\nHost: x\r\n\r\n";
    const auto first = parse(pipelined);
    expect(first.status == hp::http::ParseStatus::complete &&
               first.consumed_bytes == complete.size(),
           "only the first request header block may be consumed");

    const std::string ignored_trailing =
        std::string(complete) + std::string("\n\0ignored", 9);
    const auto ignored = parse(ignored_trailing);
    expect(ignored.status == hp::http::ParseStatus::complete &&
               ignored.consumed_bytes == complete.size(),
           "bytes after the first header block must be ignored");
}

void test_method_and_host_rules() {
    const auto post =
        parse("POST / HTTP/1.1\r\nHost: example\r\nContent-Length: 0\r\n\r\n");
    expect(post.status == hp::http::ParseStatus::method_not_allowed,
           "syntactically valid non-GET method must be 405");
    expect(post.request.method == "POST",
           "405 result must retain the parsed method");

    expect_bad("GET / HTTP/1.1\r\nUser-Agent: test\r\n\r\n",
               "missing Host must be rejected");
    expect_bad("GET / HTTP/1.1\r\nHost: a\r\nhOsT: b\r\n\r\n",
               "duplicate Host must be rejected case-insensitively");
    expect_bad("GET / HTTP/1.1\r\nHost: \t \r\n\r\n",
               "empty Host after OWS trim must be rejected");
    expect_bad("GET / HTTP/1.1\r\n Host: x\r\n\r\n",
               "obs-fold or leading whitespace must be rejected");
}

void test_request_line_and_header_syntax() {
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
        expect_bad(request, "invalid request syntax must return 400");
    }

    std::string nul_request = "GET / HTTP/1.1\r\nHost: x";
    nul_request.push_back('\0');
    nul_request += "\r\n\r\n";
    expect_bad(nul_request, "NUL in request bytes must return 400");
}

void test_limits() {
    std::string long_line = "GET /";
    long_line.append(hp::http::max_request_line_bytes, 'a');
    long_line += " HTTP/1.1\r\nHost: x\r\n\r\n";
    expect_bad(long_line, "request line over 4 KiB must return 400");

    std::string at_request_limit(hp::http::max_request_bytes, 'a');
    at_request_limit.replace(0, 4, "GET ");
    expect_bad(at_request_limit,
               "16 KiB without a header terminator must return 400");

    std::string over_request_limit = "GET / HTTP/1.1\r\nHost: x\r\nX: ";
    over_request_limit.append(hp::http::max_request_bytes, 'a');
    expect_bad(over_request_limit,
               "request bytes beyond 16 KiB must return 400");

    std::string exact_line = "GET /";
    exact_line.append(hp::http::max_request_line_bytes - 14, 'a');
    exact_line += " HTTP/1.1\r\nHost: x\r\n\r\n";
    expect(exact_line.find("\r\n") == hp::http::max_request_line_bytes,
           "exact request-line fixture must be 4 KiB");
    expect(parse(exact_line).status == hp::http::ParseStatus::complete,
           "a request line exactly at 4 KiB must be accepted");

    std::string exact_request = "GET / HTTP/1.1\r\nHost: x\r\nX-Limit: ";
    exact_request.append(hp::http::max_request_bytes - exact_request.size() - 4,
                         'a');
    exact_request += "\r\n\r\n";
    expect(exact_request.size() == hp::http::max_request_bytes,
           "exact request fixture must be 16 KiB");
    expect(parse(exact_request).status == hp::http::ParseStatus::complete,
           "a complete request exactly at 16 KiB must be accepted");
}

void test_response_and_mime() {
    const std::vector<std::byte> body{std::byte{'A'}, std::byte{0},
                                      std::byte{'B'}};
    const std::string response = bytes_to_string(hp::http::make_response(
        hp::http::Status::ok, body, "application/octet-stream"));
    expect(response.starts_with("HTTP/1.1 200 OK\r\n"),
           "200 response must have the standard status line");
    expect(response.find("Content-Length: 3\r\n") != std::string::npos,
           "binary body length must be exact");
    expect(response.find("Connection: close\r\n\r\n") != std::string::npos,
           "response must use CRLF and close semantics");
    expect(response.size() >= 3 &&
               response.substr(response.size() - 3) == std::string("A\0B", 3),
           "binary response body must preserve NUL bytes");

    const std::string method = bytes_to_string(
        hp::http::make_error_response(hp::http::Status::method_not_allowed));
    expect(method.find("Allow: GET\r\n") != std::string::npos,
           "405 must include Allow: GET");
    expect(method.ends_with("405 Method Not Allowed\n"),
           "405 body must be deterministic");

    const std::string internal = bytes_to_string(
        hp::http::make_error_response(hp::http::Status::internal_server_error));
    expect(internal.starts_with("HTTP/1.1 500 Internal Server Error\r\n"),
           "500 response construction must be covered");

    expect(
        hp::http::content_type_for_path("x.html") == "text/html; charset=utf-8",
        "html MIME must be known");
    expect(
        hp::http::content_type_for_path("x.CSS") == "text/css; charset=utf-8",
        "MIME lookup must be case-insensitive");
    expect(hp::http::content_type_for_path("x.js") == "application/javascript",
           "JavaScript MIME must be known");
    expect(
        hp::http::content_type_for_path("x.txt") == "text/plain; charset=utf-8",
        "text MIME must be known");
    expect(hp::http::content_type_for_path("x.json") == "application/json",
           "JSON MIME must be known");
    expect(hp::http::content_type_for_path("x.png") == "image/png",
           "PNG MIME must be known");
    expect(hp::http::content_type_for_path("x.jpeg") == "image/jpeg",
           "JPEG MIME must be known");
    expect(hp::http::content_type_for_path("x.gif") == "image/gif",
           "GIF MIME must be known");
    expect(hp::http::content_type_for_path("x.svg") == "image/svg+xml",
           "SVG MIME must be known");
    expect(hp::http::content_type_for_path("x.unknown") ==
               "application/octet-stream",
           "unknown extension must use octet-stream");
}

}  // namespace

int main() {
    test_fragmented_and_complete_request();
    test_method_and_host_rules();
    test_request_line_and_header_syntax();
    test_limits();
    test_response_and_mime();

    if (failures != 0) {
        std::cerr << failures << " HTTP parser assertion(s) failed\n";
        return 1;
    }
    std::cout << "HTTP parser tests passed; need_more_hits=" << need_more_hits
              << " bad_request_hits=" << bad_request_hits
              << " method_not_allowed_hits=" << method_not_allowed_hits << '\n';
    return 0;
}
