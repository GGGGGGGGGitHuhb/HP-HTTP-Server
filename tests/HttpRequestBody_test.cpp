#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "HttpConnectionHandler.h"
#include "TestCheck.h"
#include "net/ConnectionRegistry.h"

namespace hp::net {
struct ConnectionTimeoutTestAccess {
  static TcpConnection& onlyConnection(ConnectionRegistry& registry) {
    requireTestCondition(registry.connections_.size() == 1);
    return *registry.connections_.begin()->second;
  }

  static std::size_t connectionCount(const ConnectionRegistry& registry) {
    return registry.connections_.size();
  }

  static bool hasReadDeadline(const TcpConnection& connection) {
    return connection.readDeadline_.has_value();
  }

  static void expire(ConnectionRegistry& registry, int fd, TcpConnection::Identity identity) {
    registry.expireConnection(fd, identity);
  }
};
}  // namespace hp::net

namespace {
using hp::http::ParseStatus;
using hp::http::RequestParser;
using hp::net::ConnectionTimeoutTestAccess;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::string makeBodyRequest(std::string_view body, std::string_view method = "GET") {
  return std::string(method) +
         " / HTTP/1.1\r\nHost: test\r\nContent-Length: " + std::to_string(body.size()) +
         "\r\n\r\n" + std::string(body);
}

void verifyBodySchedule(const std::string& wire,
                        const std::string& body,
                        const std::vector<std::size_t>& chunks,
                        ParseStatus expected = ParseStatus::kComplete) {
  RequestParser parser;
  const auto nextWire = makeBodyRequest("next");
  std::string input = wire + nextWire;
  std::size_t consumed = 0;
  for (const auto chunk : chunks) {
    const auto offered = std::min(chunk, input.size() - consumed);
    const auto result = parser.feedRequestBytes(std::string_view(input).substr(consumed, offered));
    const auto accepted = std::min(offered, wire.size() - consumed);
    requireTestCondition(result.acceptedBytes == accepted);
    consumed += accepted;
    requireTestCondition(result.requestBytes == consumed);
    requireTestCondition(result.status ==
                         (consumed == wire.size() ? expected : ParseStatus::kNeedMore));
    if (result.status == ParseStatus::kNeedMore)
      requireTestCondition(result.request.body.empty() && result.request.method.empty());
  }
  const auto result = parser.feedRequestBytes(std::string_view(input).substr(consumed));
  requireTestCondition(result.status == expected && result.requestBytes == wire.size());
  requireTestCondition(result.acceptedBytes == wire.size() - consumed);
  requireTestCondition(result.request.body == body);
  requireTestCondition(parser.scanSteps() < wire.size() * 8);
  requireTestCondition(parser.peakBufferedBytes() <= hp::http::kMaxHeaderBytes);
  const auto repeated = parser.feedRequestBytes("ignored");
  requireTestCondition(repeated.acceptedBytes == 0 && repeated.request.body == body);
  parser.resetRequestParser();
  requireTestCondition(parser.bufferedBytes() == 0 && parser.scanSteps() == 0);
  const auto second = parser.feedRequestBytes(std::string_view(input).substr(wire.size()));
  requireTestCondition(second.status == ParseStatus::kComplete && second.request.body == "next");
  requireTestCondition(second.requestBytes == nextWire.size());
  input.assign(input.size(), 'x');
  parser.resetRequestParser();
  requireTestCondition(result.request.body == body && second.request.body == "next");
  const auto compatible = hp::http::parseRequest(wire + nextWire);
  requireTestCondition(compatible.status == expected && compatible.consumedBytes == wire.size() &&
                       compatible.request.body == body);
}

void testBodyFraming() {
  const std::vector<std::string> bodies{"",
                                        "x",
                                        std::string("a\0b\r\n\xff", 7),
                                        makeBodyRequest("nested")};
  for (const auto& body : bodies) {
    const auto wire = makeBodyRequest(body);
    for (std::size_t split = 0; split <= wire.size(); ++split)
      verifyBodySchedule(wire, body, {split});
    verifyBodySchedule(wire, body, std::vector<std::size_t>(wire.size(), 1));
  }
  const std::string body(hp::http::kMaxBodyBytes, 'b');
  const auto wire = makeBodyRequest(body);
  verifyBodySchedule(wire, body, {});
  verifyBodySchedule(wire, body, std::vector<std::size_t>(wire.size() / 127, 127));
  verifyBodySchedule(wire, body, {1, 19, 4097, 8191, 317, 65536, 233});
  verifyBodySchedule("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: \t0003 \t\r\n\r\nabc",
                     "abc",
                     {1, 17, 19, 3});
  verifyBodySchedule(makeBodyRequest("post", "POST"),
                     "post",
                     {1, 20, 15},
                     ParseStatus::kMethodNotAllowed);
  const std::string prefix = "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nX: ";
  verifyBodySchedule(
      prefix + std::string(hp::http::kMaxHeaderBytes - prefix.size() - 4, 'a') + "\r\n\r\nx",
      "x",
      {127, 2031});
  std::cout << "CASE B01-body-framing-ownership-boundaries PASS\n";
}

void expectRejectedHeaders(std::string_view headers, ParseStatus expected) {
  RequestParser parser;
  const std::string prefix = "GET / HTTP/1.1\r\nHost: x\r\n";
  const auto result = parser.feedRequestBytes(prefix + std::string(headers) + "\r\n");
  requireTestCondition(result.status == expected && result.request.body.empty());
  requireTestCondition(parser.peakBufferedBytes() <= hp::http::kMaxHeaderBytes);
  const auto terminal = parser.feedRequestBytes(makeBodyRequest("never"));
  requireTestCondition(terminal.acceptedBytes == 0 && terminal.status == expected);
  parser.resetRequestParser();
  requireTestCondition(parser.feedRequestBytes(makeBodyRequest("ok")).request.body == "ok");
}

void testRejectionPriority() {
  for (const auto value : {"1048577", "18446744073709551615"}) {
    const std::string header = "Content-Length: " + std::string(value) + "\r\n";
    expectRejectedHeaders(header, ParseStatus::kPayloadTooLarge);
    expectRejectedHeaders(header + "Transfer-Encoding: chunked\r\n", ParseStatus::kBadRequest);
    expectRejectedHeaders(header + "Expect: 100-continue\r\n", ParseStatus::kBadRequest);
    expectRejectedHeaders(header + "Content-Length: 0\r\n", ParseStatus::kBadRequest);
    expectRejectedHeaders(header + "Bad Name: x\r\n", ParseStatus::kBadRequest);
  }
  for (const auto value : {"18446744073709551616", "1,1", "+1", "-1", "", "1a"})
    expectRejectedHeaders("Content-Length: " + std::string(value) + "\r\n",
                          ParseStatus::kBadRequest);
  RequestParser parser;
  const auto unfinished =
      parser.feedRequestBytes("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 1048577\r\n");
  requireTestCondition(unfinished.status == ParseStatus::kNeedMore &&
                       unfinished.request.body.empty());
  requireTestCondition(parser.feedRequestBytes("\r\nbody").acceptedBytes == 2);
  requireTestCondition(
      hp::http::parseRequest("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 1048577\r\n\r\n")
          .consumedBytes == 0);
  std::cout << "CASE B02-rejection-priority PASS\n";
}

struct Provider {
  std::vector<hp::http::HttpRequest> requests;
  bool largeResponse{false};
  hp::http::ResponseResult onResponse(const hp::http::HttpRequest& request,
                                      hp::http::ConnectionPolicy policy);
};

hp::http::ResponseResult Provider::onResponse(const hp::http::HttpRequest& request,
                                              hp::http::ConnectionPolicy policy) {
  requests.push_back(request);
  const std::string body = largeResponse ? std::string(1024 * 1024, 'R') : request.body;
  return {hp::http::makeResponse(hp::http::Status::kOk,
                                 {reinterpret_cast<const std::byte*>(body.data()), body.size()},
                                 "application/octet-stream",
                                 false,
                                 policy),
          policy,
          {},
          hp::http::Status::kOk,
          body.size()};
}

class SessionFixture {
 public:
  explicit SessionFixture(std::chrono::milliseconds timeout = 200ms);
  void connectPeer();
  void sendInput(std::string_view input);
  std::string readResponse();
  hp::net::TcpConnection& connection();

  hp::net::EventLoop loop;
  hp::metrics::ServerMetrics metrics;
  hp::net::ConnectionRegistry registry;
  hp::net::Socket peer;
  Provider provider;
  std::chrono::milliseconds timeout;
};

SessionFixture::SessionFixture(std::chrono::milliseconds readTimeout)
    : registry(loop, hp::http::kMaxHeaderBytes, {}, &metrics), timeout(readTimeout) {
  connectPeer();
}

void SessionFixture::connectPeer() {
  int sockets[2];
  requireTestCondition(
      ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets) == 0);
  peer.reset(sockets[1]);
  registry.onAccepted(hp::net::Socket(sockets[0]),
                      hp::app::makeHttpCallback(
                          [target = &provider](const hp::http::HttpRequest& request,
                                               hp::http::ConnectionPolicy policy) {
                            return target->onResponse(request, policy);
                          },
                          nullptr,
                          &metrics,
                          false,
                          timeout));
}

hp::net::TcpConnection& SessionFixture::connection() {
  return ConnectionTimeoutTestAccess::onlyConnection(registry);
}

void SessionFixture::sendInput(std::string_view input) {
  const auto deadline = Clock::now() + 5s;
  std::size_t sent = 0;
  while (sent < input.size()) {
    requireTestCondition(Clock::now() < deadline);
    const auto count = ::send(peer.fd(), input.data() + sent, input.size() - sent, MSG_NOSIGNAL);
    if (count > 0)
      sent += static_cast<std::size_t>(count);
    else
      requireTestCondition(errno == EAGAIN || errno == EWOULDBLOCK);
    loop.pollOnce(0);
  }
}

std::string SessionFixture::readResponse() {
  std::string response;
  const auto deadline = Clock::now() + 5s;
  while (Clock::now() < deadline) {
    char bytes[16384];
    const auto count = ::recv(peer.fd(), bytes, sizeof(bytes), 0);
    if (count > 0)
      response.append(bytes, static_cast<std::size_t>(count));
    else if (count == 0)
      return response;
    else
      requireTestCondition(errno == EAGAIN || errno == EWOULDBLOCK);
    requireTestCondition(response.size() <= 9 * 1024 * 1024);
    if (count > 0) continue;
    if (ConnectionTimeoutTestAccess::connectionCount(registry) == 0 ||
        connection().pendingBytes() == 0)
      return response;
    loop.pollOnce(1);
  }
  requireTestCondition(false);
  return {};
}

void testSessionBodyAndMetrics() {
  SessionFixture fixture;
  fixture.sendInput("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\na");
  requireTestCondition(fixture.provider.requests.empty());
  requireTestCondition(fixture.loop.timerCount() == 1);
  fixture.sendInput(std::string("\0b", 2) + makeBodyRequest("second"));
  requireTestCondition(fixture.provider.requests.size() == 2);
  requireTestCondition(fixture.provider.requests[0].body == std::string("a\0b", 3));
  requireTestCondition(fixture.provider.requests[1].body == "second");
  requireTestCondition(!ConnectionTimeoutTestAccess::hasReadDeadline(fixture.connection()));
  requireTestCondition(fixture.loop.timerCount() == 0);
  fixture.readResponse();
  fixture.sendInput("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 1048577\r\n\r\n");
  const auto response = fixture.readResponse();
  requireTestCondition(response.find("413 Content Too Large\r\n") != std::string::npos);
  requireTestCondition(fixture.provider.requests.size() == 2);
  const auto snapshot = fixture.metrics.snapshot();
  requireTestCondition(snapshot.responseStatus[7] == 1 && snapshot.responseStatus[1] == 2 &&
                       snapshot.parseErrors == 1 && snapshot.errors == 1);
  requireTestCondition(snapshot.connectionsActive == 0 && snapshot.responsesCompleted == 3 &&
                       snapshot.requestsAborted == 0);
  requireTestCondition(hp::metrics::ServerMetrics::serializeSnapshot(snapshot).find(
                           "responses_status_413_total 1") != std::string::npos);
  std::cout << "CASE B03-provider-and-413-metrics PASS\n";
}

void testIncompleteAndMethods() {
  for (const auto method : {"POST", "HEAD"}) {
    SessionFixture fixture;
    fixture.sendInput(makeBodyRequest("abc", method));
    requireTestCondition(fixture.readResponse().find("405 Method Not Allowed") !=
                         std::string::npos);
    requireTestCondition(fixture.provider.requests.empty());
  }
  SessionFixture fixture;
  fixture.sendInput("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\nx");
  requireTestCondition(::shutdown(fixture.peer.fd(), SHUT_WR) == 0);
  fixture.loop.pollOnce(5);
  requireTestCondition(fixture.readResponse().find("400 Bad Request") != std::string::npos);
  requireTestCondition(fixture.provider.requests.empty() &&
                       fixture.metrics.snapshot().requestsAborted == 0);
  std::cout << "CASE B04-incomplete-and-methods PASS\n";
}

void testDeadlineLifetime() {
  SessionFixture fixture;
  fixture.sendInput("GET / HTTP/1.1\r\nHost:");
  requireTestCondition(fixture.loop.timerCount() == 1);
  const auto oldFd = fixture.connection().fd();
  const auto oldIdentity = fixture.connection().identity();
  fixture.connection().requestClose();
  fixture.registry.onCleanup();
  fixture.peer.reset();
  requireTestCondition(fixture.loop.timerCount() == 0);
  fixture.connectPeer();
  requireTestCondition(fixture.connection().fd() == oldFd);
  requireTestCondition(fixture.connection().identity() != oldIdentity);
  fixture.sendInput("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\n");
  ConnectionTimeoutTestAccess::expire(fixture.registry, oldFd, oldIdentity);
  requireTestCondition(fixture.connection().state() == hp::net::TcpConnection::State::kActive);
  fixture.sendInput("x");
  requireTestCondition(fixture.provider.requests.size() == 1 && fixture.loop.timerCount() == 0);
  fixture.readResponse();
  fixture.sendInput("GET / HTTP/1.1\r\nHost:");
  const auto deadline = Clock::now() + 2s;
  while (fixture.metrics.snapshot().connectionsActive && Clock::now() < deadline)
    fixture.loop.pollOnce(20);
  const auto snapshot = fixture.metrics.snapshot();
  requireTestCondition(snapshot.connectionsActive == 0 && snapshot.requestsAborted == 2 &&
                       snapshot.responseStatus[0] == 2);
  requireTestCondition(snapshot.responsesCompleted == 1 && fixture.loop.timerCount() == 0);
  fixture.loop.pollOnce(0);
  requireTestCondition(fixture.metrics.snapshot().requestsAborted == 2);
  std::cout << "CASE B05-deadline-identity-and-aborted-once PASS\n";
}

void testWritingAndDrain() {
  SessionFixture fixture;
  fixture.provider.largeResponse = true;
  fixture.sendInput(makeBodyRequest("x"));
  requireTestCondition(fixture.connection().pendingBytes() > 0);
  requireTestCondition(!ConnectionTimeoutTestAccess::hasReadDeadline(fixture.connection()));
  requireTestCondition(fixture.loop.timerCount() == 0);
  const auto afterDeadline = Clock::now() + 300ms;
  while (Clock::now() < afterDeadline) fixture.loop.pollOnce(10);
  requireTestCondition(fixture.connection().state() == hp::net::TcpConnection::State::kActive);
  requireTestCondition(fixture.readResponse().size() > 1024 * 1024);
  fixture.sendInput("GET / HTTP/1.1\r\nHost:");
  requireTestCondition(fixture.loop.timerCount() == 1);
  fixture.registry.beginConnectionsDrain();
  requireTestCondition(fixture.loop.timerCount() == 0 &&
                       fixture.metrics.snapshot().connectionsActive == 0);
  std::cout << "CASE B06-writing-clear-and-drain PASS\n";
}
}  // namespace

int main() {
  testBodyFraming();
  testRejectionPriority();
  testSessionBodyAndMetrics();
  testIncompleteAndMethods();
  testDeadlineLifetime();
  testWritingAndDrain();
  std::cout << "Request body and deadline baseline PASS\n";
}
