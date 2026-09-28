#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <optional>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "base/FileRegion.h"
#include "base/UniqueFd.h"
#include "net/Acceptor.h"
#include "net/ConnectionIo.h"
#include "net/EventLoop.h"
#include "net/Socket.h"

namespace {
void require(bool condition, std::source_location location = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error(std::string(location.function_name()) + ":" +
                             std::to_string(location.line()) + " check failed");
  }
}

thread_local bool failNextAccepted = false;
thread_local int failedAcceptedFd = -1;
thread_local int failedCloseCount = 0;
thread_local int injectedFailures = 0;
}  // namespace

extern "C" int __real_accept4(int, sockaddr*, socklen_t*, int);
extern "C" int __wrap_accept4(int listener, sockaddr* address, socklen_t* length, int flags) {
  const int accepted = __real_accept4(listener, address, length, flags);
  if (accepted >= 0 && failNextAccepted) {
    failNextAccepted = false;
    failedAcceptedFd = accepted;
  }
  return accepted;
}

extern "C" int __real_setsockopt(int, int, int, const void*, socklen_t);
extern "C" int __wrap_setsockopt(int socket,
                                 int level,
                                 int option,
                                 const void* value,
                                 socklen_t length) {
  if (failedAcceptedFd >= 0 && socket == failedAcceptedFd && level == IPPROTO_TCP &&
      option == TCP_NODELAY) {
    ++injectedFailures;
    errno = EIO;
    return -1;
  }
  return __real_setsockopt(socket, level, option, value, length);
}

extern "C" int __real_close(int);
extern "C" int __wrap_close(int descriptor) {
  if (descriptor == failedAcceptedFd) ++failedCloseCount;
  return __real_close(descriptor);
}

namespace hp::net {
struct AcceptorTestAccess {
  static int listenerFd(Acceptor& acceptor) { return acceptor.listener_.fd(); }
};
}  // namespace hp::net

namespace {
using hp::base::UniqueFd;
using hp::net::Socket;

UniqueFd connectClient(std::uint16_t port) {
  UniqueFd peer(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
  require(peer.get() >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(::connect(peer.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
  return peer;
}

Socket acceptClient(Socket& listener) {
  pollfd ready{listener.fd(), POLLIN, 0};
  require(::poll(&ready, 1, 2000) == 1 && (ready.revents & POLLIN));
  return listener.acceptNonBlocking();
}

int tcpNoDelay(int descriptor) {
  int enabled = -1;
  socklen_t length = sizeof(enabled);
  require(::getsockopt(descriptor, IPPROTO_TCP, TCP_NODELAY, &enabled, &length) == 0);
  require(length == sizeof(enabled));
  return enabled;
}

void testSocketOption() {
  Socket listener = Socket::createTcp();
  listener.bindAny(0);
  listener.listen(8);
  auto client = connectClient(listener.localPort());
  Socket accepted = acceptClient(listener);
  require(accepted.valid());
  const int descriptor = accepted.fd();
  accepted.setTcpNoDelay(true);
  require(tcpNoDelay(descriptor) == 1 && accepted.fd() == descriptor);
  accepted.setTcpNoDelay(false);
  require(tcpNoDelay(descriptor) == 0 && accepted.fd() == descriptor);
  Socket invalid;
  bool rejected = false;
  try {
    invalid.setTcpNoDelay(true);
  } catch (const std::system_error& error) {
    rejected = error.code().value() == EBADF &&
               std::string(error.what()).find("TCP_NODELAY") != std::string::npos;
  }
  require(rejected && !invalid.valid());
}

struct AcceptedProbe {
  int count = 0;
  std::optional<Socket> delivered;

  void onAccepted(Socket socket) {
    require(tcpNoDelay(socket.fd()) == 1);
    ++count;
    delivered.emplace(std::move(socket));
  }
};

void testAcceptorIsolation() {
  hp::net::EventLoop loop;
  hp::net::Acceptor acceptor(loop, 0);
  AcceptedProbe probe;
  acceptor.setAcceptedCallback([&probe](Socket socket) { probe.onAccepted(std::move(socket)); });
  acceptor.enableAcceptEvents();
  const int listener = hp::net::AcceptorTestAccess::listenerFd(acceptor);
  require(tcpNoDelay(listener) == 0);
  auto rejectedClient = connectClient(acceptor.boundPort());
  failNextAccepted = true;
  loop.pollOnce(100);
  require(injectedFailures == 1 && failedCloseCount == 1 && probe.count == 0);
  require(::fcntl(failedAcceptedFd, F_GETFD) == -1 && errno == EBADF);
  require(tcpNoDelay(listener) == 0 && !loop.failed());
  failedAcceptedFd = -1;  // 核验完成后解除定向计数，避免后续合法 fd 复用。
  auto healthyClient = connectClient(acceptor.boundPort());
  loop.pollOnce(100);
  require(probe.count == 1 && probe.delivered && !loop.failed());
  require(tcpNoDelay(listener) == 0);
  acceptor.closeListener();
}

void testBackpressure() {
  Socket listener = Socket::createTcp();
  listener.bindAny(0);
  listener.listen(8);
  auto client = connectClient(listener.localPort());
  Socket accepted = acceptClient(listener);
  accepted.setTcpNoDelay(true);
  const int smallBuffer = 4096;
  require(::setsockopt(accepted.fd(), SOL_SOCKET, SO_SNDBUF, &smallBuffer, sizeof(smallBuffer)) ==
          0);
  hp::net::ConnectionIo output(std::move(accepted));
  UniqueFd file(::memfd_create("s2-backpressure", MFD_CLOEXEC));
  require(file.get() >= 0);
  std::vector<std::byte> body(1024U * 1024U);
  for (std::size_t index = 0; index < body.size(); ++index) body[index] = std::byte(index % 251);
  require(::write(file.get(), body.data(), body.size()) == static_cast<ssize_t>(body.size()));
  const std::array header{std::byte{0x48}, std::byte{0x50}};
  output.queueFile(header, hp::base::FileRegion(std::move(file), 0, body.size()));
  std::size_t calls = 0;
  bool blocked = false;
  const auto began = std::chrono::steady_clock::now();
  while (calls++ < 64 && !blocked) {
    const auto result = output.writeAvailable();
    require(result.errorNumber == 0);
    blocked = result.wouldBlock;
  }
  require(blocked && output.hasPendingOutput());
  require(std::chrono::steady_clock::now() - began < std::chrono::seconds(1));
  require(::fcntl(client.get(), F_SETFL, O_NONBLOCK) == 0);
  std::vector<std::byte> received;
  std::array<std::byte, 65536> buffer{};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  std::size_t polls = 0;
  while (received.size() < body.size() + header.size()) {
    require(std::chrono::steady_clock::now() < deadline);
    pollfd descriptors[] = {
        {client.get(), POLLIN, 0},
        {output.fd(), static_cast<short>(output.hasPendingOutput() ? POLLOUT : 0), 0}};
    require(::poll(descriptors, 2, 100) >= 0);
    ++polls;
    if (descriptors[0].revents & POLLIN) {
      const auto count = ::recv(client.get(), buffer.data(), buffer.size(), 0);
      require(count > 0);
      received.insert(received.end(), buffer.begin(), buffer.begin() + count);
    }
    if ((descriptors[1].revents & POLLOUT) && output.hasPendingOutput()) {
      const auto result = output.writeAvailable();
      require(result.errorNumber == 0);
    }
  }
  require(!output.hasPendingOutput() && polls < 10000);
  require(std::equal(header.begin(), header.end(), received.begin()));
  require(std::equal(body.begin(), body.end(), received.begin() + header.size()));
  std::cout << "EAGAIN observed; recovered exact bytes; polls=" << polls << '\n';
}
}  // namespace

int main() {
  testSocketOption();
  testAcceptorIsolation();
  testBackpressure();
  std::cout << "TCP_NODELAY contract and failure isolation passed\n";
}
