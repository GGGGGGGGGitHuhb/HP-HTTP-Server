#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "TestCheck.h"
#include "http/HttpRequest.h"

namespace {
using hp::http::FeedResult;
using hp::http::ParserState;
using hp::http::ParseStatus;
using hp::http::RequestParser;

struct ProtocolCase {
  std::string id;
  std::string wire;
  ParseStatus expectedStatus;
  std::size_t terminalBytes;
  std::string method;
  std::string target;
  bool closeRequested{false};
};

std::string makeRequest(std::string_view headers) {
  return "GET / HTTP/1.1\r\nHost: example\r\n" + std::string(headers) + "\r\n";
}

void addCompleteCase(std::vector<ProtocolCase>& cases,
                     std::string id,
                     std::string wire,
                     std::string method = "GET",
                     std::string target = "/",
                     bool closeRequested = false) {
  const auto status = method == "GET" ? ParseStatus::kComplete : ParseStatus::kMethodNotAllowed;
  const auto size = wire.size();
  cases.push_back({std::move(id),
                   std::move(wire),
                   status,
                   size,
                   std::move(method),
                   std::move(target),
                   closeRequested});
}

void addBadCase(std::vector<ProtocolCase>& cases,
                std::string id,
                std::string terminalPrefix,
                std::string suffix = "\r\n") {
  const auto terminalBytes = terminalPrefix.size();
  cases.push_back({std::move(id),
                   terminalPrefix + suffix,
                   ParseStatus::kBadRequest,
                   terminalBytes,
                   {},
                   {},
                   false});
}

std::vector<ProtocolCase> makeShortCases() {
  std::vector<ProtocolCase> cases;
  addCompleteCase(cases, "P01-get", makeRequest(""));
  addCompleteCase(cases, "P02-zero", makeRequest("Content-Length: 0\r\n"));
  addCompleteCase(cases, "P02-zeros", makeRequest("content-length:\t000 \t\r\n"));
  addCompleteCase(cases,
                  "P04-close",
                  makeRequest("Connection: KEEP-ALIVE, cLoSe\r\n"),
                  "GET",
                  "/",
                  true);
  addCompleteCase(cases,
                  "P04-repeated",
                  makeRequest("Connection: close\r\nConnection: keep-alive\r\n"),
                  "GET",
                  "/",
                  true);
  addCompleteCase(cases, "P04-empty-tokens", makeRequest("Connection: , ,\r\n"));
  addCompleteCase(cases, "P01-post", "POST / HTTP/1.1\r\nHost: x\r\n\r\n", "POST");
  addCompleteCase(cases, "P01-head", "HEAD / HTTP/1.1\r\nHost: x\r\n\r\n", "HEAD");
  addCompleteCase(cases,
                  "P01-extension",
                  "CUSTOM /item?q=x HTTP/1.1\r\nHost: x\r\n\r\n",
                  "CUSTOM",
                  "/item?q=x");
  const std::vector<std::string> badLines = {"G(ET / HTTP/1.1",
                                             "GET  / HTTP/1.1",
                                             "GET / HTTP/1.0",
                                             "GET / HTTP/1.1 ",
                                             "GET http://example/ HTTP/1.1",
                                             "GET /#x HTTP/1.1",
                                             "GET /\t HTTP/1.1"};
  for (std::size_t i = 0; i < badLines.size(); ++i)
    addBadCase(cases, "P01-line-" + std::to_string(i), badLines[i] + "\r\n");
  addBadCase(cases, "P01-missing-host", "GET / HTTP/1.1\r\n\r\n");
  addBadCase(cases, "P01-empty-host", "GET / HTTP/1.1\r\nHost: \t\r\n");
  const std::vector<std::string> badHeaders = {"Host: again",
                                               "Host:",
                                               " Bad: fold",
                                               "Bad Name: x",
                                               "Bad@Name: x",
                                               "BadWithoutColon",
                                               "X: \x01",
                                               "Connection: bad(token)",
                                               "Content-Length: 1",
                                               "Content-Length: -1",
                                               "Content-Length: +0",
                                               "Content-Length: 0,0",
                                               "Content-Length: a",
                                               "Content-Length:",
                                               "Content-Length: 18446744073709551616",
                                               "Transfer-Encoding: chunked",
                                               "Expect: 100-continue"};
  for (std::size_t i = 0; i < badHeaders.size(); ++i)
    addBadCase(cases,
               "P01-P02-header-" + std::to_string(i),
               "GET / HTTP/1.1\r\nHost: x\r\n" + badHeaders[i] + "\r\n");
  addBadCase(cases,
             "P02-duplicate-cl",
             "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nContent-Length: 0\r\n");
  addBadCase(cases,
             "P02-cl-te",
             "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n");
  addBadCase(cases,
             "P02-post-framing-first",
             "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n");
  addBadCase(cases, "P03-bare-lf", "GET / HTTP/1.1\n");
  addBadCase(cases, "P03-cr-not-lf", "GET / HTTP/1.1\rX");
  addBadCase(cases, "P03-nul", std::string("GET /\0", 6));
  return cases;
}

void verifyFeedResult(const ProtocolCase& testCase,
                      const FeedResult& result,
                      std::size_t previousBytes,
                      std::size_t offeredBytes) {
  const auto accepted = std::min(offeredBytes, testCase.terminalBytes - previousBytes);
  requireTestCondition(result.acceptedBytes == accepted);
  requireTestCondition(result.requestBytes == previousBytes + accepted);
  const bool terminal = result.requestBytes == testCase.terminalBytes;
  requireTestCondition(result.status ==
                       (terminal ? testCase.expectedStatus : ParseStatus::kNeedMore));
  if (terminal && result.status != ParseStatus::kBadRequest) {
    requireTestCondition(result.request.method == testCase.method);
    requireTestCondition(result.request.target == testCase.target);
    requireTestCondition(result.request.closeRequested == testCase.closeRequested);
  }
}

void verifySchedule(const ProtocolCase& testCase, const std::vector<std::size_t>& chunks) {
  RequestParser parser;
  const auto empty = parser.feedRequestBytes({});
  requireTestCondition(empty.status == ParseStatus::kNeedMore && empty.acceptedBytes == 0 &&
                       empty.requestBytes == 0);
  const std::string wire = testCase.wire + makeRequest("");
  std::size_t consumed = 0;
  for (const auto count : chunks) {
    const auto offered = std::min(count, wire.size() - consumed);
    const auto result = parser.feedRequestBytes(std::string_view(wire).substr(consumed, offered));
    verifyFeedResult(testCase, result, consumed, offered);
    consumed += result.acceptedBytes;
  }
  const auto result = parser.feedRequestBytes(std::string_view(wire).substr(consumed));
  verifyFeedResult(testCase, result, consumed, wire.size() - consumed);
  requireTestCondition(result.requestBytes == testCase.terminalBytes);
  requireTestCondition(parser.state() == (testCase.expectedStatus == ParseStatus::kBadRequest
                                              ? ParserState::kError
                                              : ParserState::kComplete));
  const auto repeated = parser.feedRequestBytes("not another request");
  requireTestCondition(repeated.acceptedBytes == 0 &&
                       repeated.requestBytes == testCase.terminalBytes &&
                       repeated.status == testCase.expectedStatus);
  parser.resetRequestParser();
  requireTestCondition(parser.state() == ParserState::kRequestLine && !parser.pendingCr() &&
                       parser.bufferedBytes() == 0);
  const auto nextWire = makeRequest("");
  const auto next = parser.feedRequestBytes(nextWire);
  requireTestCondition(next.status == ParseStatus::kComplete &&
                       next.acceptedBytes == nextWire.size() &&
                       next.requestBytes == nextWire.size() && !next.request.closeRequested);
}

void verifyShortCases() {
  for (const auto& testCase : makeShortCases()) {
    std::cout << "CASE " << testCase.id << std::endl;
    verifySchedule(testCase, {});
    for (std::size_t split = 0; split <= testCase.wire.size(); ++split)
      verifySchedule(testCase, {split});
    verifySchedule(testCase, std::vector<std::size_t>(testCase.wire.size(), 1));
  }
}

void verifyLongCases() {
  std::vector<ProtocolCase> cases;
  const std::string target = "/" + std::string(hp::http::kMaxRequestLineBytes - 14, 'a');
  addCompleteCase(cases,
                  "P03-line-equal",
                  "GET " + target + " HTTP/1.1\r\nHost: x\r\n\r\n",
                  "GET",
                  target);
  const auto longLine = "GET " + target + "a HTTP/1.1\r\nHost: x\r\n\r\n";
  cases.push_back({"P03-line-over",
                   longLine,
                   ParseStatus::kBadRequest,
                   hp::http::kMaxRequestLineBytes + 1,
                   {},
                   {},
                   false});
  const std::string prefix = "GET / HTTP/1.1\r\nHost: x\r\nX: ";
  const auto padding = hp::http::kMaxRequestBytes - prefix.size() - 4;
  addCompleteCase(cases, "P03-total-equal", prefix + std::string(padding, 'a') + "\r\n\r\n");
  cases.push_back({"P03-total-over",
                   prefix + std::string(padding + 1, 'a') + "\r\n\r\n",
                   ParseStatus::kBadRequest,
                   hp::http::kMaxRequestBytes,
                   {},
                   {},
                   false});
  for (const auto& testCase : cases) {
    std::cout << "CASE " << testCase.id << std::endl;
    verifySchedule(testCase, {});
    verifySchedule(testCase, std::vector<std::size_t>(testCase.wire.size(), 1));
    verifySchedule(testCase, std::vector<std::size_t>(testCase.wire.size() / 127, 127));
  }
}

void verifyOwnershipAndPipeline() {
  RequestParser parser;
  std::string firstWire = "GET /owned HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  const std::string secondWire = makeRequest("");
  std::string input = firstWire + secondWire;
  const auto first = parser.feedRequestBytes(input);
  requireTestCondition(first.acceptedBytes == firstWire.size());
  parser.resetRequestParser();
  const auto second = parser.feedRequestBytes(std::string_view(input).substr(first.acceptedBytes));
  requireTestCondition(second.status == ParseStatus::kComplete &&
                       second.acceptedBytes == secondWire.size() && !second.request.closeRequested);
  input.assign(input.size(), 'x');
  parser.resetRequestParser();
  requireTestCondition(first.request.method == "GET" && first.request.target == "/owned" &&
                       first.request.closeRequested);
  requireTestCondition(second.request.target == "/");
  std::cout << "CASE P03-owning-pipeline PASS\n";
}
}  // namespace

int main() {
  verifyShortCases();
  verifyLongCases();
  verifyOwnershipAndPipeline();
  std::cout << "HTTP parser baseline PASS\n";
}
