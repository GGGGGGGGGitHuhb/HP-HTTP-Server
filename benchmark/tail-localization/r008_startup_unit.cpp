#include "tail_localization/ObservedRuntime.h"
#include "tail_localization/StartupEvidence.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

extern "C" int __wrap_getsockname(int fd, sockaddr* address, socklen_t* size) {
  auto* value = reinterpret_cast<sockaddr_in*>(address);
  *value = {};
  value->sin_family = AF_INET;
  value->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  value->sin_port = htons(18000);
  *size = sizeof(*value);
  return 0;
}

extern "C" int __wrap_getpeername(int fd, sockaddr* address, socklen_t* size) {
  __wrap_getsockname(fd, address, size);
  reinterpret_cast<sockaddr_in*>(address)->sin_port = htons(20000 + fd);
  return 0;
}

void verifyRejectedRegistration(HpS4Control* control, std::uint32_t code,
                                int socketFd, std::uint64_t lifetime) {
  bool rejected = false;
  try { tail_localization::registerConnection(socketFd, lifetime); }
  catch (const std::runtime_error&) { rejected = true; }
  assert(rejected);
  assert(__atomic_load_n(&control->firstFailure.published, __ATOMIC_ACQUIRE) == 2);
  assert(control->firstFailure.code == code);
  assert(control->firstFailure.connection.fd == socketFd);
  assert(control->firstFailure.connection.remotePort == 20000 + socketFd);
}

int main(int argc, char** argv) {
  assert(argc == 3);
  const std::string scenario = argv[1];
  const std::string directory = argv[2];
  const std::string path = directory + "/control-" + scenario + ".bin";
  const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  assert(fd >= 0);
  assert(::ftruncate(fd, sizeof(HpS4Control)) == 0);
  auto* control = static_cast<HpS4Control*>(::mmap(nullptr, sizeof(HpS4Control),
      PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
  assert(control != MAP_FAILED);
  std::memcpy(control->magic, "S4CTRL03", 8);
  control->version = 3;
  control->bytes = sizeof(HpS4Control);
  control->connections = 2;
  control->phase = HP_S4_REGISTERING;
  assert(::setenv("HP_S4_CONTROL", path.c_str(), 1) == 0);
  assert(::setenv("HP_S4_OUTPUT", directory.c_str(), 1) == 0);
  tail_localization::initializeObserver();
  if (scenario != "worker-missing") tail_localization::registerWorker(0);
  if (scenario == "worker-range") {
    bool rejected = false;
    try { tail_localization::registerWorker(4); }
    catch (const std::runtime_error&) { rejected = true; }
    assert(rejected && control->firstFailure.code == HP_S4_WORKER_RANGE);
  } else if (scenario == "worker-missing") verifyRejectedRegistration(control, HP_S4_WORKER_MISSING, 3, 1);
  else if (scenario == "limit") {
    tail_localization::registerConnection(3, 1);
    tail_localization::registerConnection(4, 2);
    verifyRejectedRegistration(control, HP_S4_CONNECTION_LIMIT, 5, 3);
  } else if (scenario == "duplicate") {
    tail_localization::registerConnection(3, 1);
    verifyRejectedRegistration(control, HP_S4_DUPLICATE_REGISTRATION, 4, 1);
  } else if (scenario == "different-owner") {
    tail_localization::registerConnection(3, 1);
    tail_localization::registerWorker(1);
    tail_localization::registerConnection(4, 1);
    assert(control->firstFailure.published == 0);
    assert(control->serverRegistered == 2);
  } else if (scenario == "closed-before-go") {
    auto context = tail_localization::registerConnection(3, 1);
    tail_localization::closeConnection(context);
    assert(control->firstFailure.code == HP_S4_CLOSED_BEFORE_GO);
  } else if (scenario == "request-before-go") {
    auto context = tail_localization::registerConnection(3, 1);
    tail_localization::exchangeCurrentConnection(&context);
    tail_localization::receiveSucceeded();
    assert(control->firstFailure.code == HP_S4_REQUEST_BEFORE_GO);
  } else if (scenario == "first-cause") {
    HpS4Connection connection{};
    connection.worker = 2;
    publishStartupFailure(control, 1, HP_S4_CONNECTION_LIMIT, &connection, 123, __atomic_load_n(&control->phase, __ATOMIC_ACQUIRE));
    publishStartupFailure(control, 0, HP_S4_CLOSED_BEFORE_GO, &connection, 456, __atomic_load_n(&control->phase, __ATOMIC_ACQUIRE));
    assert(control->firstFailure.code == HP_S4_CONNECTION_LIMIT);
    assert(control->firstFailure.savedErrno == 123);
  } else return 3;
  assert(::munmap(control, sizeof(HpS4Control)) == 0);
  assert(::close(fd) == 0);
  return 0;
}
