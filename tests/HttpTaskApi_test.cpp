#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <thread>

#include "HttpConnectionHandler.h"
#include "TestCheck.h"
#include "business/TaskService.h"
#include "http/TaskApiHandler.h"
#include "metrics/ServerMetrics.h"
#include "net/ConnectionRegistry.h"

namespace hp::business {
struct TaskServiceTestAccess {
  static void exhaustNext(TaskService& service) {
    service.nextId_ = std::numeric_limits<TaskId>::max();
  }
};
}  // namespace hp::business

namespace {
using namespace hp::http;
using namespace hp::business;

void testCodec() {
  auto input = parseTaskInput(R"({"title":"hello","completed":false})");
  requireTestCondition(input &&
                       input->title == (std::getenv("HP_TASK_API_NEGATIVE") ? "wrong" : "hello") &&
                       !input->completed);
  for (auto body : {R"({"completed":true,"title":" \"\\中文"})",
                    R"({"title":"\uD83D\uDE00","completed":true})",
                    R"({"title":" ","completed":false})"})
    requireTestCondition(parseTaskInput(body).has_value());
  const std::string valid = R"({"title":"x","completed":false})";
  requireTestCondition(parseTaskInput(valid + std::string(8192 - valid.size(), ' ')).has_value());
  requireTestCondition(!parseTaskInput(valid + std::string(8193 - valid.size(), ' ')));
  requireTestCondition(
      parseTaskInput("{\"title\":\"" + std::string(256, 'x') + "\",\"completed\":true}")
          .has_value());
  requireTestCondition(
      !parseTaskInput("{\"title\":\"" + std::string(257, 'x') + "\",\"completed\":true}"));
  for (auto body : {"",
                    "null",
                    "true",
                    "[]",
                    "{}",
                    R"({"title":"x"})",
                    R"({"completed":true})",
                    R"({"title":"","completed":false})",
                    R"({"title":"x","completed":0})",
                    R"({"title":null,"completed":false})",
                    R"({"title":"x","completed":null})",
                    R"({"title":"x","completed":true,"extra":1})",
                    R"({"title":"x","title":"y","completed":false})",
                    R"({"title":"x","tit\u006ce":"y","completed":false})",
                    R"({"title":"x","completed":false,"completed":true})",
                    R"({"title":{},"completed":false})",
                    R"({"title":[],"completed":false})",
                    R"({"title":"\u0000","completed":false})",
                    R"({"title":"\u007f","completed":false})",
                    R"({"title":"\n","completed":false})",
                    R"({"title":"\uD800","completed":false})",
                    R"({"title":"x","completed":NaN})",
                    R"({"title":"x","completed":false,})",
                    R"({/*x*/"title":"x","completed":false})"})
    requireTestCondition(!parseTaskInput(body));
  requireTestCondition(!parseTaskInput(valid + "{}") &&
                       !parseTaskInput(valid + std::string(1, '\0')));
  requireTestCondition(!parseTaskInput("\xef\xbb\xbf" + valid));
  requireTestCondition(
      !parseTaskInput(std::string("{\"title\":\"") + char(0xff) + "\",\"completed\":true}"));
  requireTestCondition(!parseTaskInput(std::string(4000, '[') + std::string(4000, ']')));
  for (auto type : {"application/json",
                    "Application/JSON ; Charset = \"UTF-8\"",
                    " application/json; charset=utf-8\t"})
    requireTestCondition(isTaskMediaType({type, true, false}));
  for (auto type : {"",
                    "text/json",
                    "application/json;",
                    "application/json; foo=x",
                    "application/json; charset=ascii",
                    "application/json; charset=utf-8; charset=utf-8",
                    "application/json,application/json",
                    "application/json; charset=\"utf-8"})
    requireTestCondition(!isTaskMediaType({type, true, false}));
  requireTestCondition(!isTaskMediaType({"application/json", false, false}) &&
                       !isTaskMediaType({"application/json", true, true}));
  std::cout << "CASE A01-SAX-schema-UTF8-NUL-BOM-8KiB-title-media PASS\n";
}

void testParser() {
  const std::string wire =
      "POST /api/tasks HTTP/1.1\r\nHost: x\r\nContent-Type: \tapplication/json \t\r\ncontent-type: "
      "other\r\nContent-Length: 1\r\n\r\nx";
  for (std::size_t split = 1; split < wire.size(); ++split) {
    RequestParser parser;
    auto result = parser.feedRequestBytes(std::string_view(wire).substr(0, split));
    requireTestCondition(result.status == ParseStatus::kNeedMore &&
                         !result.request.contentType.present);
    result = parser.feedRequestBytes(std::string_view(wire).substr(split));
    requireTestCondition(result.status == ParseStatus::kComplete &&
                         result.request.contentType.value == "application/json" &&
                         result.request.contentType.duplicate);
    parser.resetRequestParser();
    auto next = parser.feedRequestBytes("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    requireTestCondition(!next.request.contentType.present && result.request.contentType.present);
  }
  RequestParser parser;
  FeedResult result;
  for (char byte : wire) result = parser.feedRequestBytes(std::string_view(&byte, 1));
  requireTestCondition(result.request.contentType.value == "application/json");
  requireTestCondition(parseRequest("POST /api/tasks HTTP/1.1\r\nHost: x\r\nContent-Type: "
                                    "garbage\r\nContent-Length: +1\r\n\r\n")
                           .status == ParseStatus::kBadRequest);
  requireTestCondition(parseRequest("GET / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: "
                                    "chunked\r\n\r\n0\r\nContent-Type: application/json\r\n\r\n")
                           .status == ParseStatus::kBadRequest);
  std::cout << "CASE A02-content-type-owning-reset-framing-trailer PASS\n";
}

void testStore() {
  TaskService service;
  requireTestCondition(service.listTasks().empty());
  requireTestCondition(service.replaceTask(1, {"missing", false}).status == TaskStatus::kNotFound);
  auto first = service.createTask({"first", false});
  requireTestCondition(first.task->id == 1);
  auto replacement = service.replaceTask(1, {"second", true});
  requireTestCondition(replacement.task->input.completed && first.task->input.title == "first");
  requireTestCondition(service.deleteTask(1).status == TaskStatus::kSuccess &&
                       service.deleteTask(1).status == TaskStatus::kNotFound);
  requireTestCondition(service.createTask({"next", false}).task->id == 2);
  for (std::size_t i = 1; i < 128; ++i)
    requireTestCondition(service.createTask({"x", false}).status == TaskStatus::kSuccess);
  requireTestCondition(service.createTask({"x", false}).status == TaskStatus::kCapacity);
  requireTestCondition(service.deleteTask(2).status == TaskStatus::kSuccess);
  requireTestCondition(service.createTask({"next", false}).task->id == 130);
  TaskService maximum;
  TaskServiceTestAccess::exhaustNext(maximum);
  requireTestCondition(maximum.createTask({"max", false}).task->id ==
                       std::numeric_limits<TaskId>::max());
  requireTestCondition(maximum.deleteTask(std::numeric_limits<TaskId>::max()).status ==
                       TaskStatus::kSuccess);
  requireTestCondition(maximum.createTask({"none", false}).status == TaskStatus::kIdExhausted);
  std::cout << "CASE A03-CRUD-snapshot-capacity-ID-exhaustion PASS\n";
}

class ConcurrentProbe {
 public:
  void create(unsigned worker);
  void replaceAndRead(unsigned worker);
  TaskService service;
  std::barrier<> start{8};
  std::atomic<unsigned> failures{0};
  std::vector<TaskId> ids[8];
};

void ConcurrentProbe::create(unsigned worker) {
  start.arrive_and_wait();
  for (unsigned i = 0; i < 20; ++i) {
    auto result = service.createTask({"parallel", false});
    if (result.status == TaskStatus::kSuccess)
      ids[worker].push_back(result.task->id);
    else if (result.status != TaskStatus::kCapacity)
      ++failures;
  }
}

void ConcurrentProbe::replaceAndRead(unsigned worker) {
  start.arrive_and_wait();
  for (unsigned i = 0; i < 100; ++i) {
    if (worker < 4) {
      const bool flag = (worker % 2) != 0;
      if (service.replaceTask(1, {flag ? "true" : "false", flag}).status != TaskStatus::kSuccess)
        ++failures;
    } else {
      auto result = service.getTask(1);
      auto snapshot = service.listTasks();
      if (!result.task ||
          result.task->input.title != (result.task->input.completed ? "true" : "false"))
        ++failures;
      if (snapshot.size() != 1 ||
          snapshot[0].input.title != (snapshot[0].input.completed ? "true" : "false"))
        ++failures;
    }
  }
}

void testConcurrency() {
  ConcurrentProbe creation;
  std::vector<std::thread> threads;
  for (unsigned i = 0; i < 8; ++i) threads.emplace_back(&ConcurrentProbe::create, &creation, i);
  for (auto& thread : threads) thread.join();
  std::set<TaskId> unique;
  std::size_t count = 0;
  for (auto& ids : creation.ids) {
    count += ids.size();
    unique.insert(ids.begin(), ids.end());
  }
  requireTestCondition(count == 128 && unique.size() == 128 && *unique.begin() == 1 &&
                       *unique.rbegin() == 128 && creation.failures == 0);
  const auto snapshot = creation.service.listTasks();
  for (std::size_t i = 0; i < snapshot.size(); ++i) requireTestCondition(snapshot[i].id == i + 1);
  ConcurrentProbe replace;
  requireTestCondition(replace.service.createTask({"false", false}).task->id == 1);
  threads.clear();
  for (unsigned i = 0; i < 8; ++i)
    threads.emplace_back(&ConcurrentProbe::replaceAndRead, &replace, i);
  for (auto& thread : threads) thread.join();
  requireTestCondition(replace.failures == 0);
  std::cout << "CASE A04-eight-threads-unique-capacity-atomic-PUT-snapshots-joined PASS\n";
}

std::string text(const std::vector<std::byte>& bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

class RootFixture {
 public:
  RootFixture();
  ~RootFixture();
  std::filesystem::path directory;
};

RootFixture::RootFixture() {
  const auto parent = std::filesystem::path(std::getenv("HP_S3_TEST_TMP_ROOT"));
  std::filesystem::create_directories(parent);
  auto name = (parent / "task-unit-XXXXXX").string();
  requireTestCondition(::mkdtemp(name.data()));
  directory = name;
  std::ofstream(directory / "index.html") << "home";
}

RootFixture::~RootFixture() {
  std::filesystem::remove_all(directory);
  std::cout << "FIXTURE removed " << directory << '\n';
}

void testRouter(const RootFixture& root) {
  StaticFileService files(root.directory);
  auto store = std::make_shared<TaskService>();
  RequestRouter router(files, store);
  bool nullRejected = false;
  try {
    RequestRouter invalid(files, {});
  } catch (const std::invalid_argument&) {
    nullRejected = true;
  }
  requireTestCondition(nullRejected);
  HttpRequest request{"POST", "/api/tasks", false, R"({"title":"x","completed":false})"};
  request.contentType = {"application/json", true, false};
  auto created = router.onResponse(request, ConnectionPolicy::kKeepAlive);
  requireTestCondition(created.status == Status::kCreated &&
                       text(created.bytes).find("Cache-Control: no-store\r\n") !=
                           std::string::npos);
  for (auto path :
       {"/api/tasks/0", "/api/tasks/01", "/api/tasks/18446744073709551616", "/api/tasks/-1"}) {
    request.target = path;
    auto result = router.onResponse(request, ConnectionPolicy::kKeepAlive);
    requireTestCondition(result.status == Status::kBadRequest &&
                         result.effectivePolicy == ConnectionPolicy::kKeepAlive);
  }
  for (auto path : {"/api/tasks/0", "/api/tasks?x", "/api/tasks/1/more"}) {
    for (auto method : {"GET", "HEAD"}) {
      const auto early =
          router.onRequestHeaders({method, path, false}, ConnectionPolicy::kKeepAlive);
      const auto complete =
          router.onResponse({method, path, false, "x"}, ConnectionPolicy::kKeepAlive);
      requireTestCondition(early && early->status == complete.status &&
                           early->effectivePolicy == ConnectionPolicy::kClose &&
                           complete.effectivePolicy == ConnectionPolicy::kKeepAlive);
      requireTestCondition(text(early->bytes).find("Connection: close\r\n") != std::string::npos &&
                           text(complete.bytes).find("Connection: keep-alive\r\n") !=
                               std::string::npos);
      if (std::string_view(method) == "HEAD")
        requireTestCondition(early->contentBytes == 0 && complete.contentBytes == 0);
    }
  }
  auto head = router.onResponse({"HEAD", "/api/tasks/1", false, {}}, ConnectionPolicy::kKeepAlive);
  requireTestCondition(head.status == Status::kMethodNotAllowed && head.contentBytes == 0 &&
                       text(head.bytes).ends_with("\r\n\r\n") &&
                       text(head.bytes).find("Allow: GET, PUT, DELETE") != std::string::npos);
  requireTestCondition(
      !router.onRequestHeaders({"POST", "/api/tasks", false}, ConnectionPolicy::kKeepAlive));
  requireTestCondition(store->listTasks().size() == 1);
  request.target = "/api/tasks/999";
  request.method = "PUT";
  request.body = "bad";
  requireTestCondition(router.onResponse(request, ConnectionPolicy::kKeepAlive).status ==
                           Status::kBadRequest &&
                       store->listTasks().size() == 1);
  // 业务公共接口可由测试注入非法UTF8，验证HTTP边界受控异常，不暴露异常文本。
  auto invalid = store->createTask({std::string(1, char(0xff)), false});
  request = {"GET", "/api/tasks/" + std::to_string(invalid.task->id), false, {}};
  auto caught = router.onResponse(request, ConnectionPolicy::kKeepAlive);
  requireTestCondition(caught.status == Status::kInternalServerError &&
                       caught.effectivePolicy == ConnectionPolicy::kClose &&
                       text(caught.bytes).find("internal_error") != std::string::npos &&
                       text(caught.bytes).find("Cache-Control: no-store") != std::string::npos);
  std::cout << "CASE A05-router-policy-HEAD-admission-no-side-effect-catch500 PASS\n";
}

std::string factoryExchange(hp::net::EventLoop& loop,
                            hp::net::ConnectionRegistry& registry,
                            const hp::net::TcpServer::MessageFactoryCallback& factory,
                            std::string_view wire) {
  int sockets[2];
  requireTestCondition(
      ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets) == 0);
  hp::net::Socket peer(sockets[1]);
  registry.onAccepted(hp::net::Socket(sockets[0]), factory());
  requireTestCondition(::send(peer.fd(), wire.data(), wire.size(), MSG_NOSIGNAL) ==
                       static_cast<ssize_t>(wire.size()));
  std::string output;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  char bytes[8192];
  bool closed = false;
  while (std::chrono::steady_clock::now() < deadline) {
    loop.pollOnce(0);
    const auto count = ::recv(peer.fd(), bytes, sizeof(bytes), 0);
    if (count > 0)
      output.append(bytes, static_cast<std::size_t>(count));
    else if (count == 0) {
      closed = true;
      break;
    } else
      requireTestCondition(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
    requireTestCondition(output.size() <= kMaxTaskResponseBytes + 4096);
  }
  requireTestCondition(closed);
  return output;
}

void testFactory(const RootFixture& root) {
  StaticFileService files(root.directory);
  hp::net::EventLoop loop;
  hp::net::ConnectionRegistry registry(loop, kMaxHeaderBytes);
  auto first = hp::app::makeHttpFactory(files);
  auto second = hp::app::makeHttpFactory(files);
  const std::string body = R"({"title":"shared","completed":false})";
  const auto post =
      "POST /api/tasks HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Type: "
      "application/json\r\nContent-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;
  requireTestCondition(factoryExchange(loop, registry, first, post).starts_with("HTTP/1.1 201 "));
  const std::string get = "GET /api/tasks HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  requireTestCondition(factoryExchange(loop, registry, first, get).find("shared") !=
                       std::string::npos);
  requireTestCondition(factoryExchange(loop, registry, second, get).ends_with("{\"tasks\":[]}"));
  std::cout << "CASE A06-real-sockets-factory-shared-and-independent PASS\n";
}

void testMetrics() {
  hp::metrics::ServerMetrics metrics;
  for (int status : {200, 400, 403, 404, 405, 500, 413, 417, 501, 206, 304, 416, 201, 415, 503}) {
    metrics.beginRequest();
    metrics.finishRequest(false, status, 1);
  }
  const auto result = metrics.snapshot();
  requireTestCondition(result.responseStatus.size() == 16 && result.responseStatus[0] == 0);
  for (std::size_t i = 1; i < 16; ++i) requireTestCondition(result.responseStatus[i] == 1);
  requireTestCondition(result.responsesCompleted == 15 && result.errors == 11 &&
                       result.parseErrors == 0 && result.providerErrors == 0);
  std::cout << "CASE A07-original-indices-new201-415-503 PASS\n";
}
}  // namespace

int main() {
  testCodec();
  testParser();
  testStore();
  testConcurrency();
  {
    RootFixture root;
    testRouter(root);
    testFactory(root);
  }
  testMetrics();
  std::cout << "Task API unit PASS\n";
}
