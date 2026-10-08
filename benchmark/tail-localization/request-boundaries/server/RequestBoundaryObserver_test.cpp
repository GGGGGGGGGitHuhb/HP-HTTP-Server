#include "RequestBoundaryObserver.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
struct SocketPair {
  int listener = -1;
  int client = -1;
  int server = -1;

  SocketPair() {
    listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(listener >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    assert(::listen(listener, 1) == 0);
    socklen_t size = sizeof(address);
    assert(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) == 0);
    client = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(client >= 0);
    assert(::connect(client, reinterpret_cast<sockaddr*>(&address), size) == 0);
    server = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    assert(server >= 0);
  }

  ~SocketPair() {
    if (server >= 0) ::close(server);
    if (client >= 0) ::close(client);
    if (listener >= 0) ::close(listener);
  }
};
}  // namespace

int main() {
  using hp::net::RequestBoundaryObserver;
  SocketPair sockets;
  RequestBoundaryObserver observer(0, 2);
  observer.initializeWriter(RequestBoundaryObserver::processStarttime());
  auto* connection = observer.registerConnection(sockets.server);
  assert(connection && connection->clientAddress == INADDR_LOOPBACK);
  observer.observeRead(connection, 100);
  observer.observeRead(connection, 120);  // 分片和EAGAIN恢复不新增首读。
  assert(observer.recordCount() == 1 && observer.record(0).readNs == 100);
  observer.observeParsed(connection);
  observer.observeDrained(connection, 200);
  assert(observer.record(0).flags == 3 && observer.record(0).drainedNs == 200);
  observer.observeRead(connection, 300);
  assert(observer.record(1).sequence == 2);
  observer.observeClosed(connection, 350);  // 末请求未完成仍保留首读。
  auto* newLifetime = observer.registerConnection(sockets.server);
  assert(newLifetime && newLifetime->connectionId == 2 && newLifetime->fd == connection->fd);
  observer.observeClosed(newLifetime, RequestBoundaryObserver::monotonicNs());
  observer.stopWriter();
  assert(observer.record(1).flags == 1 && observer.record(1).drainedNs == 0);
  const char* roleTmp = std::getenv("TMPDIR");
  char path[4096];
  assert(roleTmp);
  assert(std::snprintf(path, sizeof(path), "%s/hp-r018-unit-XXXXXX", roleTmp) < static_cast<int>(sizeof(path)));
  assert(::mkdtemp(path));
  const int directory = ::open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  assert(directory >= 0);
  assert(observer.exportRecords(directory));
  const int records = ::openat(directory, "boundary-worker-0.bin", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  assert(records >= 0);
  unsigned char bytes[256]{};
  assert(::read(records, bytes, sizeof(bytes)) == 256);
  assert(std::memcmp(bytes, "HPBOUND1", 8) == 0);
  assert(bytes[96] == 1 && bytes[100] == 3 && bytes[104] == 1 && bytes[112] == 100);
  assert(std::memcmp(bytes + 160, "HPBTAIL1", 8) == 0);
  ::close(records);
  ::close(directory);

  RequestBoundaryObserver bounded(1, 1);
  bounded.initializeWriter(RequestBoundaryObserver::processStarttime());
  auto* repeatedFd = bounded.registerConnection(sockets.server);
  bounded.observeRead(repeatedFd, 400);
  bounded.observeParsed(repeatedFd);
  bounded.observeDrained(repeatedFd, 500);
  bounded.observeRead(repeatedFd, 600);
  assert(!bounded.valid() && bounded.recordCount() == 1);
  assert(std::strcmp(bounded.invalidReason(), "record_capacity") == 0);
  bounded.observeDrained(repeatedFd, 700);
  assert(std::strcmp(bounded.invalidReason(), "record_capacity") == 0);  // 首因不覆盖。
  assert(connection != repeatedFd);  // fd不是跨observer生命期身份。

  RequestBoundaryObserver unsupported(2, 4);
  unsupported.initializeWriter(RequestBoundaryObserver::processStarttime());
  auto* pipeline = unsupported.registerConnection(sockets.server);
  unsupported.observeRead(pipeline, 800);
  unsupported.observeParsed(pipeline);
  unsupported.observeParsed(pipeline);
  assert(!unsupported.valid());
  assert(unsupported.record(0).flags == 1);

  RequestBoundaryObserver measured(3, 1000);
  measured.initializeWriter(RequestBoundaryObserver::processStarttime());
  auto* measuredConnection = measured.registerConnection(sockets.server);
  const auto begin = std::chrono::steady_clock::now();
  for (unsigned index = 0; index < 1000; ++index) {
    const auto read = RequestBoundaryObserver::monotonicNs();
    measured.observeRead(measuredConnection, read);
    measured.observeParsed(measuredConnection);
    measured.observeDrained(measuredConnection, RequestBoundaryObserver::monotonicNs());
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
  assert(measured.valid() && measured.recordCount() == 1000);
  std::printf("{\"status\":\"valid\",\"clock_record_operations\":1000,\"elapsed_ns\":%lld,\"scope\":\"sanitized_unit_not_load_perturbation\"}\n", static_cast<long long>(elapsed));
}
