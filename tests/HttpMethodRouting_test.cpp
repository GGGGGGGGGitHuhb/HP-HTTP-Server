#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "HttpConnectionHandler.h"
#include "TestCheck.h"

namespace {
using hp::http::ConnectionPolicy;
using hp::http::HttpRequest;
using hp::http::ParseStatus;
using hp::http::Status;
using namespace std::chrono_literals;

std::string makeRequest(std::string_view method,
                        std::string_view path = "/",
                        std::string_view body = "") {
  return std::string(method) + " " + std::string(path) +
         " HTTP/1.1\r\nHost: test\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" +
         std::string(body);
}

std::string responseText(const hp::http::ResponseResult& response) {
  return {reinterpret_cast<const char*>(response.bytes.data()), response.bytes.size()};
}

void testMethodParser() {
  const std::string body("a\0\xff\r\n", 5);
  for (const auto method : {"GET", "HEAD", "POST", "get", "head", "FROB", "X!+"}) {
    const auto wire = makeRequest(method, "/api/echo", body);
    for (std::size_t split = 0; split <= wire.size(); ++split) {
      hp::http::RequestParser parser;
      const auto first = parser.feedRequestBytes(std::string_view(wire).substr(0, split));
      requireTestCondition(
          first.status == (split == wire.size() ? ParseStatus::kComplete : ParseStatus::kNeedMore));
      if (first.status == ParseStatus::kNeedMore)
        requireTestCondition(first.request.method.empty() && first.request.body.empty());
      const auto result = parser.feedRequestBytes(wire.substr(split) + makeRequest("GET"));
      requireTestCondition(result.status == ParseStatus::kComplete &&
                           result.request.method == method && result.request.body == body);
      requireTestCondition(result.acceptedBytes == wire.size() - split &&
                           result.requestBytes == wire.size());
      requireTestCondition(parser.isHeadRequest() == (std::string_view(method) == "HEAD"));
      parser.resetRequestParser();
      requireTestCondition(!parser.isHeadRequest() && result.request.body == body);
    }
    hp::http::RequestParser byteParser;
    for (char byte : wire) {
      const auto step = byteParser.feedRequestBytes(std::string_view(&byte, 1));
      requireTestCondition(step.acceptedBytes == 1);
    }
    requireTestCondition(byteParser.state() == hp::http::ParserState::kComplete);
    const auto compatible = hp::http::parseRequest(wire);
    const bool negative = std::getenv("HP_METHOD_ROUTING_NEGATIVE") != nullptr;
    requireTestCondition(compatible.status ==
                         (negative ? ParseStatus::kBadRequest : ParseStatus::kComplete));
  }
  for (const auto wire :
       {"HEAD / HTTP/1.0\r\n", "HEAD / HTTP/1.1 extra\r\n", "H(EAD / HTTP/1.1\r\n"}) {
    hp::http::RequestParser parser;
    requireTestCondition(parser.feedRequestBytes(wire).status == ParseStatus::kBadRequest);
    requireTestCondition(!parser.isHeadRequest());
  }
  hp::http::RequestParser parser;
  requireTestCondition(parser.feedRequestBytes("HEAD / HTTP/1.1\r\nHost:").status ==
                       ParseStatus::kNeedMore);
  requireTestCondition(parser.isHeadRequest());
  requireTestCondition(parser.feedRequestBytes("\r\n\r\n").status == ParseStatus::kBadRequest &&
                       parser.isHeadRequest());
  std::cout << "CASE M01-parser-token-splits-ownership-head-trust PASS\n";
}

class RootFixture {
 public:
  RootFixture();
  ~RootFixture();
  std::filesystem::path root;
};

RootFixture::RootFixture() {
  const auto parent =
      std::filesystem::path(std::getenv("HP_S3_TEST_TMP_ROOT") ? std::getenv("HP_S3_TEST_TMP_ROOT")
                                                               : std::getenv("TMPDIR"));
  std::filesystem::create_directories(parent);
  auto name = (parent / "method-unit-XXXXXX").string();
  requireTestCondition(::mkdtemp(name.data()) != nullptr);
  root = name;
  std::filesystem::create_directory(root / "api");
  std::ofstream(root / "index.html") << "home\n";
  std::ofstream(root / "api/echo") << "shadow";
  std::ofstream(root / "empty");
  std::ofstream(root / "large");
  std::filesystem::resize_file(root / "large", hp::http::kMaxFileBytes);
  std::filesystem::create_symlink(root / "index.html", root / "link");
}

RootFixture::~RootFixture() {
  std::filesystem::remove_all(root);
  std::cout << "FIXTURE removed " << root << '\n';
}

std::size_t fdCount() {
  return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                       std::filesystem::directory_iterator{});
}

void checkResponse(const hp::http::ResponseResult& result,
                   Status status,
                   std::size_t length,
                   bool head,
                   std::string_view allow = "") {
  const auto text = responseText(result);
  requireTestCondition(result.status == status);
  requireTestCondition(text.find("Content-Length: " + std::to_string(length) + "\r\n") !=
                       std::string::npos);
  const auto boundary = text.find("\r\n\r\n");
  requireTestCondition(boundary != std::string::npos);
  if (head)
    requireTestCondition(!result.file && result.contentBytes == 0 && boundary + 4 == text.size());
  if (allow.empty())
    requireTestCondition(text.find("Allow:") == std::string::npos);
  else {
    const auto start = text.find("Allow: " + std::string(allow) + "\r\n");
    requireTestCondition(start != std::string::npos &&
                         text.find("Allow:", start + 1) == std::string::npos);
    requireTestCondition(result.effectivePolicy == ConnectionPolicy::kClose);
  }
}

void testRouterAndHead(const hp::http::RequestRouter& router) {
  for (const auto method : {"GET", "HEAD", "POST", "get", "FROB"}) {
    for (const auto path : {"/api/echo", "/api/echo?q=plain"}) {
      auto result = router.onResponse({method, path, false, "data"}, ConnectionPolicy::kKeepAlive);
      const bool post = std::string_view(method) == "POST";
      checkResponse(result,
                    post ? Status::kOk : Status::kMethodNotAllowed,
                    post ? 4 : hp::http::errorContentBytes(Status::kMethodNotAllowed),
                    std::string_view(method) == "HEAD",
                    post ? "" : "POST");
      if (post)
        requireTestCondition(responseText(result).ends_with("\r\n\r\ndata") && !result.file);
    }
  }
  const auto before = fdCount();
  for (const auto& [path, status, length] :
       std::vector<std::tuple<std::string, Status, std::size_t>>{
           {"/", Status::kOk, 5},
           {"/empty", Status::kOk, 0},
           {"/large", Status::kOk, hp::http::kMaxFileBytes},
           {"/link", Status::kForbidden, hp::http::errorContentBytes(Status::kForbidden)},
           {"/missing", Status::kNotFound, hp::http::errorContentBytes(Status::kNotFound)},
           {"/api/echo/", Status::kForbidden, hp::http::errorContentBytes(Status::kForbidden)},
           {"/api/missing", Status::kNotFound, hp::http::errorContentBytes(Status::kNotFound)}}) {
    auto head = router.onResponse({"HEAD", path, false, {}}, ConnectionPolicy::kKeepAlive);
    auto get = router.onResponse({"GET", path, false, {}}, ConnectionPolicy::kKeepAlive);
    checkResponse(head, status, length, true);
    checkResponse(get, status, length, false);
    const auto getHeader = responseText(get).substr(0, responseText(get).find("\r\n\r\n") + 4);
    requireTestCondition(responseText(head) == getHeader);
  }
  requireTestCondition(fdCount() == before);
  for (const auto method : {"POST", "FROB", "get"}) {
    auto result = router.onResponse({method, "/missing", false, {}}, ConnectionPolicy::kKeepAlive);
    checkResponse(result,
                  Status::kMethodNotAllowed,
                  hp::http::errorContentBytes(Status::kMethodNotAllowed),
                  false,
                  "GET, HEAD");
  }
  for (const auto method : {"POST", "HEAD", "FROB"}) {
    auto result =
        router.onResponse({method, "/api/echo?q=%20", false, {}}, ConnectionPolicy::kKeepAlive);
    checkResponse(result,
                  Status::kBadRequest,
                  hp::http::errorContentBytes(Status::kBadRequest),
                  std::string_view(method) == "HEAD");
    requireTestCondition(result.effectivePolicy == ConnectionPolicy::kClose);
  }
  for (const auto& body : {std::string{},
                           std::string("a\0\xff\r\n", 5),
                           makeRequest("HEAD"),
                           std::string(hp::http::kMaxBodyBytes, 'x')}) {
    HttpRequest request{"POST", "/api/echo", false, body};
    auto result = router.onResponse(request, ConnectionPolicy::kKeepAlive);
    request.body.assign("destroy borrowed input");
    checkResponse(result, Status::kOk, body.size(), false);
    requireTestCondition(result.contentBytes == body.size() && !result.file);
    requireTestCondition(responseText(result).substr(responseText(result).find("\r\n\r\n") + 4) ==
                         body);
  }
  std::cout << "CASE M02-router-table M03-echo-ownership M04-head-metadata-fd PASS\n";
}

void ignoreClose(int, hp::net::TcpConnection::Identity) {}

class SessionFixture {
 public:
  SessionFixture(hp::app::ResponseCallback provider);
  std::string exchange(std::string_view wire, bool eof = false);

  hp::metrics::ServerMetrics metrics;
  hp::net::EventLoop loop;
  hp::net::Socket peer;
  std::unique_ptr<hp::net::TcpConnection> connection;
};

SessionFixture::SessionFixture(hp::app::ResponseCallback provider) {
  int sockets[2];
  requireTestCondition(
      ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets) == 0);
  peer.reset(sockets[1]);
  connection = std::make_unique<hp::net::TcpConnection>(loop,
                                                        hp::net::Socket(sockets[0]),
                                                        1,
                                                        hp::http::kMaxHeaderBytes);
  connection->setCloseCallback([](int fd, auto identity) { ignoreClose(fd, identity); });
  connection->setMessageCallback(hp::app::makeHttpCallback(std::move(provider), nullptr, &metrics));
  connection->activateConnection();
}

std::string SessionFixture::exchange(std::string_view wire, bool eof) {
  requireTestCondition(::send(peer.fd(), wire.data(), wire.size(), MSG_NOSIGNAL) ==
                       static_cast<ssize_t>(wire.size()));
  if (eof) requireTestCondition(::shutdown(peer.fd(), SHUT_WR) == 0);
  std::string result;
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    loop.pollOnce(1);
    char bytes[65536];
    const auto count = ::recv(peer.fd(), bytes, sizeof(bytes), 0);
    if (count > 0)
      result.append(bytes, static_cast<std::size_t>(count));
    else if (!result.empty() && connection->pendingBytes() == 0)
      return result;
  }
  throw std::runtime_error("session response timeout");
}

hp::http::ResponseResult throwProvider(const HttpRequest&, ConnectionPolicy) {
  throw std::runtime_error("injected provider failure");
}

hp::http::ResponseResult relaxPolicy(const HttpRequest&, ConnectionPolicy) {
  return {{}, ConnectionPolicy::kKeepAlive, {}, Status::kOk, 0};
}

struct CountingRouter {
  const hp::http::RequestRouter& router;
  int calls{};
  hp::http::ResponseResult onResponse(const HttpRequest& request, ConnectionPolicy policy);
};

hp::http::ResponseResult CountingRouter::onResponse(const HttpRequest& request,
                                                    ConnectionPolicy policy) {
  ++calls;
  return router.onResponse(request, policy);
}

void testSessionErrors(const hp::http::RequestRouter& router) {
  for (auto provider : {throwProvider, relaxPolicy}) {
    for (const auto method : {"HEAD", "GET"}) {
      SessionFixture fixture(provider);
      const auto wire = std::string(method) + " / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
      const auto response = fixture.exchange(wire + makeRequest("GET"));
      requireTestCondition(response.find("500 Internal Server Error\r\n") != std::string::npos);
      const auto body = response.substr(response.find("\r\n\r\n") + 4);
      requireTestCondition(
          body == (std::string_view(method) == "HEAD" ? "" : "500 Internal Server Error\n"));
      const auto snapshot = fixture.metrics.snapshot();
      requireTestCondition(snapshot.requestsStarted == 1 && snapshot.responsesCompleted == 1 &&
                           snapshot.providerErrors == 1 && snapshot.parseErrors == 0);
    }
  }
  for (const auto& [wire, status, eof, head] :
       std::vector<std::tuple<std::string, Status, bool, bool>>{
           {"HEAD / HTTP/1.1\r\n\r\n", Status::kBadRequest, false, true},
           {"HEAD / HTTP/1.1\r\nHost:x\r\nContent-Length:1048577\r\n\r\n",
            Status::kContentTooLarge,
            false,
            true},
           {"HEAD / HTTP/1.1\r\nHost:x\r\nContent-Length:2\r\n\r\nx",
            Status::kBadRequest,
            true,
            true},
           {"HEAD / HTTP/1.0\r\n", Status::kBadRequest, false, false},
           {"POST /api/echo HTTP/1.1\r\nHost:x\r\nContent-Length:1048577\r\n\r\n",
            Status::kContentTooLarge,
            false,
            false},
           {"POST /api/echo HTTP/1.1\r\nHost:x\r\nContent-Length:+1\r\n\r\n",
            Status::kBadRequest,
            false,
            false},
           {"POST /api/echo HTTP/1.1\r\nHost:x\r\nTransfer-Encoding:chunked, chunked\r\n\r\n",
            Status::kBadRequest,
            false,
            false},
           {"POST /api/echo "
            "HTTP/1.1\r\nHost:x\r\nExpect:100-continue\r\nExpect:100-continue\r\n\r\n",
            Status::kBadRequest,
            false,
            false},
           {"POST /api/echo HTTP/1.1\r\nHost:x\r\nContent-Length:2\r\n\r\nx",
            Status::kBadRequest,
            true,
            false}}) {
    CountingRouter provider{router};
    SessionFixture fixture([&provider](const auto& request, auto policy) {
      return provider.onResponse(request, policy);
    });
    const auto response = fixture.exchange(wire, eof);
    requireTestCondition(provider.calls == 0);
    requireTestCondition(response.find("HTTP/1.1 " + std::to_string(static_cast<int>(status))) ==
                         0);
    requireTestCondition(response.size() - response.find("\r\n\r\n") - 4 ==
                         (head ? 0 : hp::http::errorContentBytes(status)));
    const auto snapshot = fixture.metrics.snapshot();
    requireTestCondition(snapshot.parseErrors == 1 && snapshot.requestsStarted == 1 &&
                         snapshot.responsesCompleted == 1);
  }
  CountingRouter provider{router};
  SessionFixture fixture([&provider](const auto& request, auto policy) {
    return provider.onResponse(request, policy);
  });
  const auto response = fixture.exchange(makeRequest("POST", "/missing") + makeRequest("GET"));
  requireTestCondition(response.find("405 Method Not Allowed") != std::string::npos &&
                       provider.calls == 1);
  requireTestCondition(fixture.metrics.snapshot().parseErrors == 0 &&
                       fixture.metrics.snapshot().errors == 1);
  std::cout << "CASE M05-session-error-priority-provider M06-accounting PASS\n";
}
}  // namespace

int main() {
  testMethodParser();
  RootFixture fixture;
  hp::http::StaticFileService service(fixture.root.string());
  hp::http::RequestRouter router(service);
  testRouterAndHead(router);
  testSessionErrors(router);
  std::cout << "Method routing baseline PASS\n";
}
