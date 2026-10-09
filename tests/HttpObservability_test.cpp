#include <sys/socket.h>
#include <unistd.h>

#include <condition_variable>
#include <iostream>
#include <limits>
#include <mutex>
#include <new>
#include <sstream>
#include <stdexcept>
#include <streambuf>

#include "AccessLog.h"
#include "HttpConnectionHandler.h"
#include "TestCheck.h"
#include "base/AsyncLogger.h"
#include "net/ConnectionRegistry.h"

namespace hp::base {
struct AsyncLoggerTestAccess {
  static std::unique_ptr<AsyncLogger> createLogger(std::size_t slots, std::ostream& sink) {
    return std::unique_ptr<AsyncLogger>(new AsyncLogger(slots, sink));
  }
};
}  // namespace hp::base

namespace {
thread_local bool failNextAllocation = false;
bool throwSubmission = false;
hp::base::AsyncLogger* submissionLogger = nullptr;
}  // namespace

extern "C" void* __real__Znwm(std::size_t);
extern "C" void* __wrap__Znwm(std::size_t size) {
  if (failNextAllocation) {
    failNextAllocation = false;
    throw std::bad_alloc();
  }
  return __real__Znwm(size);
}

extern "C" void __real__ZN2hp4base4infoESt17basic_string_viewIcSt11char_traitsIcEE(
    std::string_view);
extern "C" void __wrap__ZN2hp4base4infoESt17basic_string_viewIcSt11char_traitsIcEE(
    std::string_view text) {
  if (throwSubmission) throw std::runtime_error("injected submission failure");
  if (submissionLogger)
    submissionLogger->submitLogRecord(hp::base::LogLevel::kInfo, text);
  else
    __real__ZN2hp4base4infoESt17basic_string_viewIcSt11char_traitsIcEE(text);
}

namespace {
using hp::metrics::ServerMetrics;
using hp::net::TcpConnection;

void ignoreClose(int, TcpConnection::Identity) {}

struct SocketFixture {
  hp::net::EventLoop loop;
  hp::net::Socket peer;
  std::unique_ptr<TcpConnection> connection;

  SocketFixture(ServerMetrics& metrics, hp::app::ResponseCallback provider, bool access = false) {
    int pair[2];
    requireTestCondition(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) ==
                         0);
    peer.reset(pair[1]);
    connection = std::make_unique<TcpConnection>(loop, hp::net::Socket(pair[0]), 1, 65536);
    connection->setCloseCallback([](int fd, auto identity) { ignoreClose(fd, identity); });
    connection->setMessageCallback(
        hp::app::makeHttpCallback(std::move(provider), nullptr, &metrics, access));
    connection->activateConnection();
  }

  void write(std::string_view request) {
    requireTestCondition(::send(peer.fd(), request.data(), request.size(), MSG_NOSIGNAL) ==
                         static_cast<ssize_t>(request.size()));
    loop.pollOnce(0);
  }

  std::string drain() {
    std::string response;
    char bytes[16384];
    for (int index = 0; index < 1000; ++index) {
      auto count = ::recv(peer.fd(), bytes, sizeof(bytes), 0);
      if (count > 0) response.append(bytes, static_cast<std::size_t>(count));
      if (connection->pendingBytes() == 0) return response;
      loop.pollOnce(1);
    }
    throw std::runtime_error("response did not drain");
  }
};

hp::http::ResponseResult respond(const hp::http::HttpRequest&, hp::http::ConnectionPolicy policy) {
  const std::string body = "observed";
  return {hp::http::makeResponse(hp::http::Status::kOk,
                                 {reinterpret_cast<const std::byte*>(body.data()), body.size()},
                                 "text/plain",
                                 false,
                                 policy),
          policy,
          {},
          hp::http::Status::kOk,
          body.size()};
}

hp::http::ResponseResult throwProvider(const hp::http::HttpRequest&, hp::http::ConnectionPolicy) {
  throw std::runtime_error("injected provider failure");
}

hp::http::ResponseResult relaxPolicy(const hp::http::HttpRequest& request,
                                     hp::http::ConnectionPolicy) {
  return respond(request, hp::http::ConnectionPolicy::kKeepAlive);
}

hp::http::ResponseResult largeResponse(const hp::http::HttpRequest&,
                                       hp::http::ConnectionPolicy policy) {
  const std::string body(1024 * 1024, 'R');
  return {hp::http::makeResponse(hp::http::Status::kOk,
                                 {reinterpret_cast<const std::byte*>(body.data()), body.size()},
                                 "text/plain",
                                 false,
                                 policy),
          policy,
          {},
          hp::http::Status::kOk,
          body.size()};
}

void testProvidersAndPipeline() {
  for (auto provider : {throwProvider, relaxPolicy}) {
    ServerMetrics metrics;
    SocketFixture fixture(metrics, provider);
    fixture.write("GET / HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    auto output = fixture.drain();
    requireTestCondition(output.find("500 Internal Server Error") != std::string::npos);
    auto result = metrics.snapshot();
    requireTestCondition(result.requestsStarted == 1 && result.responsesCompleted == 1);
    requireTestCondition(result.providerErrors == 1 && result.errors == 1 &&
                         result.responseStatus[6] == 1);
  }
  ServerMetrics metrics;
  SocketFixture fixture(metrics, respond);
  fixture.write(
      "GET /a HTTP/1.1\r\nHost: test\r\n\r\nGET /b HTTP/1.1\r\nHost: test\r\nConnection: "
      "close\r\n\r\n");
  fixture.drain();
  auto result = metrics.snapshot();
  requireTestCondition(result.requestsStarted == 2 && result.responsesCompleted == 2 &&
                       result.requestsAborted == 0);
}

void testGracefulDrainCompleted() {
  ServerMetrics metrics;
  SocketFixture fixture(metrics, largeResponse);
  fixture.write(
      "GET /slow HTTP/1.1\r\nHost: test\r\n\r\nGET /suffix HTTP/1.1\r\nHost: test\r\n\r\n");
  requireTestCondition(fixture.connection->pendingBytes() > 0);
  requireTestCondition(metrics.snapshot().requestsStarted == 1);
  fixture.connection->beginConnectionDrain();
  requireTestCondition(fixture.connection->isDraining());
  auto body = fixture.drain();
  char tail[16384];
  for (;;) {
    const auto count = ::recv(fixture.peer.fd(), tail, sizeof(tail), 0);
    if (count <= 0) break;
    body.append(tail, static_cast<std::size_t>(count));
  }
  requireTestCondition(body.find("200 OK") != std::string::npos);
  const auto headerEnd = body.find("\r\n\r\n");
  requireTestCondition(headerEnd != std::string::npos);
  requireTestCondition(body.substr(headerEnd + 4) == std::string(1024 * 1024, 'R'));
  const auto result = metrics.snapshot();
  requireTestCondition(result.requestsStarted == 1 && result.responsesCompleted == 1);
  requireTestCondition(result.requestsAborted == 0 && result.errors == 0);
  requireTestCondition(fixture.connection->state() == TcpConnection::State::kClosing);
  // 已排空或重复drain不得制造第二次完成，也不开始缓冲区中的suffix。
  fixture.connection->beginConnectionDrain();
  requireTestCondition(metrics.snapshot().responsesCompleted == 1 &&
                       metrics.snapshot().requestsStarted == 1);
}

void testAbortAndUniqueTerminal() {
  ServerMetrics partial;
  {
    SocketFixture fixture(partial, respond);
    fixture.write("GET / HTTP/1.1\r\nHost: test\r\n");
    fixture.write("X-Partial: value");
    requireTestCondition(partial.snapshot().requestsStarted == 1);
    fixture.connection->requestClose();
  }
  requireTestCondition(partial.snapshot().requestsAborted == 1 &&
                       partial.snapshot().responseStatus[0] == 1);
  ServerMetrics slow;
  {
    SocketFixture fixture(slow, largeResponse, true);
    fixture.write("GET /slow?secret HTTP/1.1\r\nHost: test\r\n\r\n");
    requireTestCondition(fixture.connection->pendingBytes() > 0);
    requireTestCondition(slow.snapshot().responsesCompleted == 0);
    fixture.connection->requestClose();
  }
  requireTestCondition(slow.snapshot().requestsAborted == 1 &&
                       slow.snapshot().responseStatus[1] == 1);
  ServerMetrics unique;
  hp::app::Session session(nullptr, &unique, true);
  session.requestPending = true;
  session.requestStarted = std::chrono::steady_clock::now();
  unique.beginRequest();
  throwSubmission = true;
  session.finishPendingRequest(false);
  session.finishPendingRequest(true);
  throwSubmission = false;
  requireTestCondition(unique.snapshot().responsesCompleted == 1 &&
                       unique.snapshot().requestsAborted == 0);
  requireTestCondition(unique.snapshot().accessLogFailures == 1);
  failNextAllocation = true;
  hp::app::submitAccessRecord({}, &unique);
  requireTestCondition(!failNextAllocation && unique.snapshot().accessLogFailures == 2);
}

void testRegistry() {
  ServerMetrics metrics;
  hp::net::EventLoop loop;
  hp::net::Socket peer;
  {
    hp::net::ConnectionRegistry registry(loop, 65536, {}, &metrics);
    int pair[2];
    requireTestCondition(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) ==
                         0);
    peer.reset(pair[1]);
    const auto serverFd = pair[0];
    registry.onAccepted(hp::net::Socket(serverFd), {});
    requireTestCondition(metrics.snapshot().connectionsTotal == 1 &&
                         metrics.snapshot().connectionsActive == 1);
    registry.onClose(serverFd, 1);
    registry.onClose(serverFd, 1);
    registry.onCleanup();
    requireTestCondition(metrics.snapshot().connectionsActive == 0);
    int nextPair[2];
    requireTestCondition(
        socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, nextPair) == 0);
    peer.reset(nextPair[1]);
    registry.onAccepted(hp::net::Socket(nextPair[0]), {});
    requireTestCondition(metrics.snapshot().connectionsActive == 1);
    bool registrationFailed = false;
    try {
      registry.onAccepted(hp::net::Socket{}, {});
    } catch (...) {
      registrationFailed = true;
    }
    requireTestCondition(registrationFailed && metrics.snapshot().connectionsActive == 1);
    registry.beginConnectionsDrain(true);
    requireTestCondition(metrics.snapshot().connectionsActive == 0);
    int rejected[2];
    requireTestCondition(
        socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, rejected) == 0);
    peer.reset(rejected[1]);
    registry.onAccepted(hp::net::Socket(rejected[0]), {});
    requireTestCondition(metrics.snapshot().connectionsTotal == 2);
  }
  requireTestCondition(metrics.snapshot().connectionsActive == 0 &&
                       metrics.snapshot().connectionsTotal == 2);
  requireTestCondition(metrics.snapshot().requestsStarted == 0);
}

void testJsonBounds() {
  hp::app::AccessRecord record;
  std::string path(2000, '\x80');
  record.copyRequest("\"\\\r\n", path + "?secret#fragment");
  record.contentBytes = record.durationUs = std::numeric_limits<std::uint64_t>::max();
  auto text = hp::app::serializeAccessRecord(record);
  requireTestCondition(text.size() <= hp::base::AsyncLogger::kMessageLimit);
  requireTestCondition(text.find("secret") == std::string::npos &&
                       text.find("\\u0080") != std::string::npos);
  requireTestCondition(text.find('\n') == std::string::npos && record.pathTruncated);
}

class GatedSink : public std::streambuf {
 public:
  std::mutex mutex;
  std::condition_variable changed;
  bool entered{}, released{};

  void waitForWrite() {
    std::unique_lock lock(mutex);
    changed.wait(lock, [this] { return entered; });
  }

  void releaseWrite() {
    const std::lock_guard lock(mutex);
    released = true;
    changed.notify_all();
  }

 protected:
  std::streamsize xsputn(const char*, std::streamsize) override {
    std::unique_lock lock(mutex);
    entered = true;
    changed.notify_all();
    changed.wait(lock, [this] { return released; });
    return 0;
  }
};

void testLoggerFailureDoesNotChangeHttp() {
  GatedSink sink;
  std::ostream output(&sink);
  auto logger = hp::base::AsyncLoggerTestAccess::createLogger(1, output);
  submissionLogger = logger.get();
  hp::app::AccessRecord record;
  hp::app::submitAccessRecord(record, nullptr);
  sink.waitForWrite();
  ServerMetrics metrics;
  {
    SocketFixture fixture(metrics, respond, true);
    fixture.write("GET / HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    requireTestCondition(fixture.drain().find("200 OK") != std::string::npos);
  }
  // 消费线程受控停在首次write，唯一队列槽已由HTTP占据。
  hp::app::submitAccessRecord(record, &metrics);
  requireTestCondition(logger->stats().droppedFull == 1);
  sink.releaseWrite();
  logger->stopAsyncLogging();
  requireTestCondition(logger->stats().failed == 2 && logger->stats().pending == 0);
  requireTestCondition(logger->stats().truncated == 0 &&
                       metrics.snapshot().responsesCompleted == 1);
  submissionLogger = nullptr;
  throwSubmission = true;
  {
    SocketFixture fixture(metrics, respond, true);
    fixture.write("GET / HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    requireTestCondition(fixture.drain().find("200 OK") != std::string::npos);
  }
  throwSubmission = false;
  requireTestCondition(metrics.snapshot().responsesCompleted == 2 &&
                       metrics.snapshot().accessLogFailures == 1);
}
}  // namespace

int main(int argc, char* argv[]) {
  if (argc == 2 && std::string_view(argv[1]) == "--json") {
    hp::app::AccessRecord record;
    std::string input;
    for (int byte = 0; byte < 256; ++byte) input += static_cast<char>(byte);
    record.copyRequest(input, input + "?secret#secret");
    std::cout << hp::app::serializeAccessRecord(record) << '\n';
    record.copyRequest(std::string(16, '\x80'), std::string(2000, '\x80'));
    record.contentBytes = record.durationUs = std::numeric_limits<std::uint64_t>::max();
    std::cout << hp::app::serializeAccessRecord(record) << '\n';
    return 0;
  }
  testProvidersAndPipeline();
  testGracefulDrainCompleted();
  testAbortAndUniqueTerminal();
  testRegistry();
  testJsonBounds();
  testLoggerFailureDoesNotChangeHttp();
  std::cout << "session/provider/pipeline/pending-abort/registry/JSON/submit/full/write failure: "
               "passed\n";
}
