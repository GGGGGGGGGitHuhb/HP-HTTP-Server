#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "HttpConnectionHandler.h"
#include "TestCheck.h"
#include "net/ConnectionRegistry.h"

namespace hp::net {
struct ConnectionTimeoutTestAccess {
  static TcpConnection& connection(ConnectionRegistry& registry) {
    requireTestCondition(registry.connections_.size() == 1);
    return *registry.connections_.begin()->second;
  }

  static std::size_t count(const ConnectionRegistry& registry) {
    return registry.connections_.size();
  }

  static bool blocked(const TcpConnection& connection) {
    return connection.lastResult_.writeWouldBlock;
  }
};
}  // namespace hp::net

namespace {
using hp::http::ParseStatus;
using hp::http::RequestParser;
using hp::net::ConnectionTimeoutTestAccess;
using namespace std::chrono_literals;

std::string headers(std::string_view fields = "Transfer-Encoding: chunked\r\n") {
  return "POST /api/echo HTTP/1.1\r\nHost: test\r\n" + std::string(fields) + "\r\n";
}

void checkStatus(std::string_view wire, ParseStatus expected) {
  const auto result = hp::http::parseRequest(wire);
  requireTestCondition(result.status == expected);
  if (expected != ParseStatus::kComplete) requireTestCondition(result.consumedBytes == 0);
}

void checkSchedule(const std::string& wire, const std::string& body, std::size_t step) {
  RequestParser parser;
  std::size_t offset = 0;
  const auto input = wire + "GET / HTTP/1.1\r\nHost:x\r\n\r\n";
  hp::http::FeedResult result;
  while (offset < wire.size()) {
    const auto count = std::min(step, input.size() - offset);
    result = parser.feedRequestBytes(std::string_view(input).substr(offset, count));
    requireTestCondition(result.acceptedBytes > 0 && result.acceptedBytes <= count);
    offset += result.acceptedBytes;
    requireTestCondition(result.requestBytes == offset);
    if (offset < wire.size()) requireTestCondition(result.request.body.empty());
  }
  requireTestCondition(offset == wire.size() && result.status == ParseStatus::kComplete &&
                       result.request.body == body);
  requireTestCondition(parser.scanSteps() < wire.size() * 10);
  parser.resetRequestParser();
  const auto next = parser.feedRequestBytes(std::string_view(input).substr(offset));
  requireTestCondition(next.status == ParseStatus::kComplete && result.request.body == body);
  const auto compatible = hp::http::parseRequest(input);
  requireTestCondition(compatible.status == ParseStatus::kComplete &&
                       compatible.consumedBytes == wire.size() && compatible.request.body == body);
}

void testHeaderMatrix() {
  const auto status = hp::http::parseRequest(headers() + "0\r\n\r\n").status;
  requireTestCondition(status == (std::getenv("HP_CHUNKED_EXPECT_NEGATIVE")
                                      ? ParseStatus::kBadRequest
                                      : ParseStatus::kComplete));
  for (const auto fields : {"Transfer-Encoding: ChUnKeD \t\r\n",
                            "Transfer-Encoding:\tchunked\r\nTrailer: X-Checksum, X-Trace-Id\r\n"})
    checkStatus(headers(fields) + "0\r\n\r\n", ParseStatus::kComplete);
  for (const auto fields :
       {"Transfer-Encoding: chunked\r\nContent-Length: 0\r\n",
        "Transfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n",
        "Transfer-Encoding: chunked, chunked\r\n",
        "Transfer-Encoding: chunked,gzip\r\n",
        "Transfer-Encoding: gzip\r\n",
        "Transfer-Encoding: ,chunked\r\n",
        "Transfer-Encoding: chunked,\r\n",
        "Transfer-Encoding: chunked;x=1\r\n",
        "Transfer-Encoding: g(zip,chunked\r\n",
        "Expect: \r\n",
        "Expect:100-continue\r\nExpect:100-continue\r\n",
        "Trailer: X-Checksum\r\n",
        "Transfer-Encoding:chunked\r\nTrailer: Host\r\n",
        "Transfer-Encoding:chunked\r\nTrailer: X-Checksum, x-checksum\r\n",
        "Transfer-Encoding:chunked\r\nTrailer: X-Trace-Id,\r\n",
        "Transfer-Encoding:chunked\r\nTrailer: X-Trace-Id\r\nTrailer:X-Trace-Id\r\n"})
    checkStatus(headers(fields), ParseStatus::kBadRequest);
  checkStatus(headers("Transfer-Encoding: gzip, chunked\r\nExpect: other\r\n"),
              ParseStatus::kNotImplemented);
  for (const auto value : {"other", "100-continue, other", "100-continue;x=1"})
    checkStatus(headers(std::string("Expect:") + value + "\r\n"), ParseStatus::kExpectationFailed);
  checkStatus(headers("Content-Length:1048577\r\nExpect:other\r\n"), ParseStatus::kPayloadTooLarge);
  checkStatus(headers("Content-Length:1048577\r\nExpect:100-continue\r\n"),
              ParseStatus::kPayloadTooLarge);
  for (const auto fields :
       {"Content-Length:1048577\r\n", "Transfer-Encoding:gzip,chunked\r\n", "Expect:other\r\n"})
    checkStatus(headers(std::string(fields) + "Bad Header:x\r\n"), ParseStatus::kBadRequest);
  checkStatus("POST / HTTP/1.1\r\nExpect:other\r\n\r\n", ParseStatus::kBadRequest);
  std::cout << "CASE C01-header-matrix-priority PASS\n";
}

void testChunkSyntax() {
  const std::string body("a\0\xff", 3);
  const auto wire =
      headers() + "0003 ; name = \"a\\\"b\"; flag; empty=\"\"\r\n" + body +
      "\r\n00;last=ok\r\nX-Checksum: ignored\r\nx-checksum: twice\r\nX-Trace-Id: trace\r\n\r\n";
  for (std::size_t split = 0; split <= wire.size(); ++split) {
    RequestParser parser;
    const auto first = parser.feedRequestBytes(std::string_view(wire).substr(0, split));
    const auto second = parser.feedRequestBytes(wire.substr(split) + "suffix");
    requireTestCondition(first.acceptedBytes == split &&
                         second.acceptedBytes == wire.size() - split);
    requireTestCondition(second.status == ParseStatus::kComplete && second.request.body == body);
  }
  checkSchedule(wire, body, 1);
  checkStatus(headers() + std::string("1;x=\"\x80\"\r\nx\r\n0\r\n\r\n"), ParseStatus::kComplete);
  for (const auto size : {"",
                          "+1",
                          "-1",
                          "0x1",
                          "10000000000000000",
                          "1;",
                          "1;x=",
                          "1;=x",
                          "1;x=\"bad",
                          "1;x=\"a\"oops",
                          "1;x=\"\x01\"",
                          "1;x=\"a\\\x01\"",
                          "1 ",
                          "1;x=token "})
    checkStatus(headers() + std::string(size) + "\r\nx\r\n0\r\n\r\n", ParseStatus::kBadRequest);
  for (const auto tail : {"1\nx",
                          "1\rX",
                          "1\r\nxX",
                          "1\r\nx\rX",
                          "0\r\n folded:x\r\n\r\n",
                          "0\r\nBad Name:x\r\n\r\n",
                          "0\r\nX-Checksum:\x01\r\n\r\n"})
    checkStatus(headers() + tail, ParseStatus::kBadRequest);
  for (const auto field :
       {"Host", "Content-Length", "Transfer-Encoding", "Connection", "Expect", "Target", "Other"})
    checkStatus(headers() + "0\r\n" + field + ": x\r\n\r\n", ParseStatus::kBadRequest);
  checkStatus(headers() + "100001;bad=\"\r\n", ParseStatus::kBadRequest);
  checkStatus(headers() + "100001\r\n", ParseStatus::kPayloadTooLarge);
  checkStatus(headers() + "ffffffffffffffff\r\n", ParseStatus::kPayloadTooLarge);
  std::cout << "CASE C02-splits-extensions C03-trailer-security PASS\n";
}

void testBoundaries() {
  const std::string body(hp::http::kMaxBodyBytes, 'B');
  const auto wire = headers() + "100000\r\n" + body + "\r\n0\r\n\r\n";
  checkSchedule(wire, body, wire.size() + 50);
  checkSchedule(wire, body, 7919);
  checkStatus(headers() + "80000\r\n" + body.substr(0, body.size() / 2) + "\r\n80001\r\n",
              ParseStatus::kPayloadTooLarge);
  checkSchedule(headers() + "80000\r\n" + body.substr(0, body.size() / 2) + "\r\n80000\r\n" +
                    body.substr(0, body.size() / 2) + "\r\n0\r\n\r\n",
                body,
                4093);
  checkStatus(headers() + "1;x=" + std::string(1018, 'a') + "\r\nx\r\n0\r\n\r\n",
              ParseStatus::kComplete);
  checkStatus(headers() + "1;x=" + std::string(1019, 'a') + "\r\nx\r\n0\r\n\r\n",
              ParseStatus::kBadRequest);
  for (const auto extra : {0, 1}) {
    auto chunks = headers();
    for (int i = 0; i < 13105; ++i) chunks += "1\r\nx\r\n";
    chunks += "0;a=" + std::string(3 + extra, 'a') + "\r\n\r\n";
    checkStatus(chunks, extra ? ParseStatus::kBadRequest : ParseStatus::kComplete);
    const auto trailer =
        headers() + "0\r\nX-Checksum: " + std::string(8176 + extra, 'a') + "\r\n\r\n";
    checkStatus(trailer, extra ? ParseStatus::kBadRequest : ParseStatus::kComplete);
  }
  std::cout << "CASE C04-body-line-metadata-trailer-exact-limits PASS\n";
}

void testHeaderEvent() {
  const auto head = headers("Content-Length:3\r\nExpect: 100-CoNtInUe \t\r\n");
  RequestParser parser;
  const auto event = parser.feedRequestBytes(head + "abcNEXT");
  requireTestCondition(event.status == ParseStatus::kHeadersComplete &&
                       event.acceptedBytes == head.size() && event.head &&
                       event.head->method == "POST" && event.request.body.empty());
  requireTestCondition(parser.feedRequestBytes("").status == ParseStatus::kNeedMore);
  const auto final = parser.feedRequestBytes("abcNEXT");
  requireTestCondition(final.status == ParseStatus::kComplete && final.acceptedBytes == 3 &&
                       final.request.body == "abc" && !final.head);
  parser.resetRequestParser();
  requireTestCondition(event.head->target == "/api/echo");
  checkStatus(head, ParseStatus::kNeedMore);
  checkStatus(head + "abc", ParseStatus::kComplete);
  checkStatus(headers("Content-Length:0\r\nExpect:100-continue\r\n"), ParseStatus::kComplete);
  checkStatus(headers("Expect:100-continue\r\n"), ParseStatus::kComplete);
  std::cout << "CASE C05-one-head-event-owned-compatible PASS\n";
}

struct Provider {
  unsigned calls{};

  hp::http::ResponseResult onResponse(const hp::http::HttpRequest& request,
                                      hp::http::ConnectionPolicy policy) {
    ++calls;
    return {hp::http::makeResponse(hp::http::Status::kOk,
                                   std::as_bytes(std::span(request.body)),
                                   "application/octet-stream",
                                   false,
                                   policy),
            policy,
            {},
            hp::http::Status::kOk,
            request.body.size()};
  }
};

class SessionFixture {
 public:
  SessionFixture();
  void sendInput(std::string_view wire);
  std::string receiveOutput(std::size_t expected);
  std::size_t fillSocket();
  void closeSession();
  hp::net::TcpConnection& connection() { return ConnectionTimeoutTestAccess::connection(registry); }

  hp::metrics::ServerMetrics metrics;
  hp::net::EventLoop loop;
  hp::net::ConnectionRegistry registry;
  hp::net::Socket peer;
  Provider provider;
  std::shared_ptr<hp::app::Session> session;
};

SessionFixture::SessionFixture()
    : registry(loop, hp::http::kMaxHeaderBytes, {}, &metrics),
      session(std::make_shared<hp::app::Session>(nullptr, &metrics, false, 300ms)) {
  int sockets[2];
  requireTestCondition(
      ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets) == 0);
  int size = 4096;
  requireTestCondition(::setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0);
  peer.reset(sockets[1]);
  session->setResponseCallback([target = &provider](const auto& request, auto policy) {
    return target->onResponse(request, policy);
  });
  registry.onAccepted(
      hp::net::Socket(sockets[0]),
      [target = session](hp::net::TcpConnection& connection, std::span<const std::byte>, bool) {
        target->onMessage(connection);
      });
  connection().setWriteCompleteCallback([target = session](hp::net::TcpConnection& connection) {
    target->onWriteComplete(connection);
  });
}

void SessionFixture::sendInput(std::string_view wire) {
  requireTestCondition(::send(peer.fd(), wire.data(), wire.size(), MSG_NOSIGNAL) ==
                       static_cast<ssize_t>(wire.size()));
  loop.pollOnce(0);
}

std::size_t SessionFixture::fillSocket() {
  std::string bytes(4096, 'P');
  std::size_t total = 0;
  while (true) {
    auto count = ::send(connection().fd(), bytes.data(), bytes.size(), MSG_NOSIGNAL);
    if (count < 0) {
      requireTestCondition(errno == EAGAIN || errno == EWOULDBLOCK);
      break;
    }
    total += static_cast<std::size_t>(count);
  }
  requireTestCondition(total > 0);
  return total;
}

std::string SessionFixture::receiveOutput(std::size_t expected) {
  std::string output;
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (output.size() < expected) {
    requireTestCondition(std::chrono::steady_clock::now() < deadline);
    char bytes[16384];
    const auto count = ::recv(peer.fd(), bytes, sizeof(bytes), 0);
    if (count > 0)
      output.append(bytes, static_cast<std::size_t>(count));
    else
      requireTestCondition(count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    loop.pollOnce(1);
  }
  return output;
}

void SessionFixture::closeSession() {
  registry.beginConnectionsDrain(true);
  registry.onCleanup();
  session.reset();
}

void testInterimLifecycle() {
  const std::string interim = "HTTP/1.1 100 Continue\r\n\r\n";
  const auto head = headers("Expect:100-continue\r\nContent-Length:3\r\n");
  for (bool blocked : {false, true}) {
    SessionFixture fixture;
    const auto padding = blocked ? fixture.fillSocket() : 0;
    fixture.sendInput(head + "abc");
    if (blocked) {
      requireTestCondition(fixture.session->phase == hp::app::Session::Phase::kInterimWriting &&
                           fixture.connection().pendingBytes() == interim.size());
      requireTestCondition(ConnectionTimeoutTestAccess::blocked(fixture.connection()));
      requireTestCondition(fixture.provider.calls == 0 &&
                           fixture.metrics.snapshot().responsesCompleted == 0 &&
                           fixture.session->readDeadline.has_value());
      std::cout << "EAGAIN actual padding=" << padding
                << " pending=" << fixture.connection().pendingBytes() << '\n';
    }
    const auto finalSize = hp::http::makeResponseHeader(hp::http::Status::kOk,
                                                        3,
                                                        "application/octet-stream",
                                                        false,
                                                        hp::http::ConnectionPolicy::kKeepAlive)
                               .size() +
                           3;
    const auto output = fixture.receiveOutput(padding + interim.size() + finalSize);
    const std::string combined = output.substr(padding);
    requireTestCondition(combined.starts_with(interim) &&
                         combined.find(interim, interim.size()) == std::string::npos &&
                         combined.ends_with("abc"));
    requireTestCondition(
        fixture.provider.calls == 1 && fixture.metrics.snapshot().requestsStarted == 1 &&
        fixture.metrics.snapshot().responsesCompleted == 1 && !fixture.session->readDeadline);
    fixture.closeSession();
    requireTestCondition(fixture.metrics.snapshot().connectionsActive == 0 &&
                         fixture.metrics.snapshot().requestsAborted == 0);
  }
  for (bool drain : {false, true}) {
    SessionFixture fixture;
    const auto padding = fixture.fillSocket();
    fixture.sendInput(head);
    requireTestCondition(fixture.connection().pendingBytes() == interim.size());
    const auto originalDeadline = fixture.session->readDeadline;
    requireTestCondition(originalDeadline.has_value());
    if (drain) {
      fixture.registry.beginConnectionsDrain();
      const auto output = fixture.receiveOutput(padding + interim.size());
      requireTestCondition(output.substr(padding) == interim);
      fixture.loop.pollOnce(0);
      requireTestCondition(fixture.session->phase == hp::app::Session::Phase::kClosing);
    } else {
      while (std::chrono::steady_clock::now() < *originalDeadline + 20ms) fixture.loop.pollOnce(10);
      requireTestCondition(ConnectionTimeoutTestAccess::count(fixture.registry) == 0);
      requireTestCondition(fixture.session->readDeadline == originalDeadline);
    }
    fixture.closeSession();
    const auto result = fixture.metrics.snapshot();
    requireTestCondition(fixture.provider.calls == 0 && result.requestsStarted == 1 &&
                         result.responsesCompleted == 0 && result.requestsAborted == 1 &&
                         result.responseStatus[0] == 1 && result.connectionsActive == 0);
  }
  std::cout << "CASE C06-real-EAGAIN-sync-final-timeout-drain-accounting PASS\n";
}
}  // namespace

int main() {
  testHeaderMatrix();
  testChunkSyntax();
  testBoundaries();
  testHeaderEvent();
  testInterimLifecycle();
  std::cout << "Chunked Expect baseline PASS\n";
}
