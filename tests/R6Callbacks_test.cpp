#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <cerrno>
#include <future>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <thread>

#include "HttpConnectionHandler.h"
#include "base/UniqueFd.h"
#include "net/EventLoopThread.h"

namespace {
thread_local int allocationCountdown = -1;
thread_local std::atomic<bool>* waitForDestruction = nullptr;
}  // namespace

extern "C" void* __real__Znwm(std::size_t size);
extern "C" void* __wrap__Znwm(std::size_t size) {
  if (allocationCountdown == 0) {
    allocationCountdown = -1;
    throw std::bad_alloc();
  }
  if (allocationCountdown > 0) --allocationCountdown;
  return __real__Znwm(size);
}

extern "C" int __real_pthread_mutex_unlock(pthread_mutex_t* mutex);
extern "C" int __wrap_pthread_mutex_unlock(pthread_mutex_t* mutex) {
  const auto result = __real_pthread_mutex_unlock(mutex);
  if (waitForDestruction) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!waitForDestruction->load() &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    assert(waitForDestruction->load());
  }
  return result;
}

namespace hp::net {
struct EventLoopTestAccess {
  static bool captureStillCounted(EventLoop& loop) {
    std::lock_guard lock(loop.mutex_);
    return loop.outstanding_ == 1;
  }
};
}  // namespace hp::net

namespace {
using hp::net::EventLoop;
using hp::net::EventLoopThread;
using hp::net::TcpConnection;

void ignoreEvent(std::uint32_t) {}
void ignoreClose(int, TcpConnection::Identity) {}
void ignoreTask(EventLoop&) {}

struct RegistrationProbe {
  EventLoop& loop;
  bool dispatchControlRejected{false};
  bool dispatchCleanupRejected{false};
  bool threadControlRejected{false};
  bool threadCleanupRejected{false};

  void probeDispatch(EventLoop::Control, EventLoop::Deadline) {
    try {
      loop.registerControlCallback({});
    } catch (const std::logic_error&) {
      dispatchControlRejected = true;
    }
    try {
      loop.registerCleanupCallback({});
    } catch (const std::logic_error&) {
      dispatchCleanupRejected = true;
    }
  }

  void probeThread() {
    try {
      loop.registerControlCallback({});
    } catch (const std::logic_error&) {
      threadControlRejected = true;
    }
    try {
      loop.registerCleanupCallback({});
    } catch (const std::logic_error&) {
      threadCleanupRejected = true;
    }
  }
};

void testRegistration() {
  EventLoop loop;
  hp::base::UniqueFd fd(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
  assert(fd.get() >= 0);
  hp::net::Channel channel(loop, fd.get());
  bool missing = false;
  try {
    channel.setInterest(EPOLLIN);
  } catch (const std::logic_error&) {
    missing = true;
  }
  assert(missing);
  channel.registerEventCallback(
      [](std::uint32_t events) { ignoreEvent(events); });
  channel.setInterest(EPOLLIN);
  bool active = false;
  try {
    channel.registerEventCallback({});
  } catch (const std::logic_error&) {
    active = true;
  }
  assert(active);
  channel.remove();
  channel.registerEventCallback({});

  RegistrationProbe probe{loop};
  std::thread wrongOwner(&RegistrationProbe::probeThread, &probe);
  wrongOwner.join();
  assert(probe.threadControlRejected && probe.threadCleanupRejected);
  loop.registerControlCallback([&probe](auto control, auto deadline) {
    probe.probeDispatch(control, deadline);
  });
  loop.requestDrain(EventLoop::Deadline::max());
  loop.pollOnce(0);
  assert(probe.dispatchControlRejected && probe.dispatchCleanupRejected);
  loop.registerControlCallback({});
  loop.registerCleanupCallback({});

  hp::net::TcpServer server(0);
  server.requestStop();
  server.run();
  bool ran = false;
  try {
    server.registerMessageFactoryCallback({});
  } catch (const std::logic_error&) {
    ran = true;
  }
  assert(ran);
}

struct SocketFixture {
  EventLoop& loop;
  hp::net::Socket peer;
  std::unique_ptr<TcpConnection> connection;

  SocketFixture(EventLoop& owner, std::uint64_t id) : loop(owner) {
    int pair[2];
    assert(socketpair(AF_UNIX,
                      SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                      0,
                      pair) == 0);
    peer.reset(pair[1]);
    connection = std::make_unique<TcpConnection>(loop,
                                                 hp::net::Socket(pair[0]),
                                                 id,
                                                 65536);
    connection->setCloseCallback(
        [](int fd, auto identity) { ignoreClose(fd, identity); });
  }

  void write(std::string_view text) {
    assert(::send(peer.fd(), text.data(), text.size(), MSG_NOSIGNAL) ==
           static_cast<ssize_t>(text.size()));
    loop.pollOnce(0);
  }

  std::string readAll() {
    std::string result;
    char buffer[16384];
    for (;;) {
      const auto n = ::recv(peer.fd(), buffer, sizeof(buffer), 0);
      if (n > 0)
        result.append(buffer, static_cast<std::size_t>(n));
      else {
        assert(n == 0 || errno == EAGAIN || errno == EWOULDBLOCK);
        break;
      }
    }
    return result;
  }
};

void testEcho(bool clearCallback) {
  EventLoop loop;
  SocketFixture fixture(loop, 1);
  if (clearCallback) fixture.connection->setMessageCallback({});
  fixture.connection->start();
  fixture.write("echo-r6");
  assert(fixture.readAll() == "echo-r6");
}

struct Gate {
  std::promise<void> entered;
  std::promise<void> release;
  std::shared_future<void> released{release.get_future().share()};

  void wait(EventLoop&) {
    entered.set_value();
    released.wait();
  }
};

struct CaptureProbe {
  EventLoopThread& worker;
  std::atomic<int>& destroyed;
  std::atomic<int>& executed;

  ~CaptureProbe() {
    // Reenters the very mutex held by post. CTest timeout catches lock-held
    // release.
    worker.requestStop();
    ++destroyed;
  }

  void run(EventLoop&) { ++executed; }
};

EventLoopThread::TaskCallback makeProbe(EventLoopThread& worker,
                                        std::atomic<int>& destroyed,
                                        std::atomic<int>& executed) {
  auto probe = std::make_shared<CaptureProbe>(worker, destroyed, executed);
  return [probe](EventLoop& loop) { probe->run(loop); };
}

struct SuccessfulCapture {
  EventLoopThread& worker;
  EventLoop& loop;
  std::atomic<bool>& destructionStarted;
  std::atomic<bool>& ownerObserved;
  std::atomic<bool>& quotaObserved;

  ~SuccessfulCapture() {
    ownerObserved = loop.isInLoopThread();
    destructionStarted = true;
    // The producer is held just after the queue mutex unlock, while post
    // still owns its outer mutex. Capture release must begin on the owner.
    quotaObserved = hp::net::EventLoopTestAccess::captureStillCounted(loop);
    worker.requestStop();
  }

  void run(EventLoop&) {}
};

void publishLoop(std::promise<EventLoop*>& ready, EventLoop& loop) {
  ready.set_value(&loop);
}

void testImmediateOwnerDestruction() {
  EventLoopThread worker;
  std::promise<EventLoop*> ready;
  worker.start([&ready](EventLoop& loop) { publishLoop(ready, loop); });
  auto* loop = ready.get_future().get();
  std::atomic<bool> started{false}, owner{false}, counted{false};
  auto capture = std::make_shared<SuccessfulCapture>(worker,
                                                     *loop,
                                                     started,
                                                     owner,
                                                     counted);
  EventLoopThread::TaskCallback task = [capture](EventLoop& ownerLoop) {
    capture->run(ownerLoop);
  };
  capture.reset();
  waitForDestruction = &started;
  assert(worker.post(std::move(task)));
  waitForDestruction = nullptr;
  worker.join();
  assert(owner && counted);
}

void testTaskSuccessAndStopped() {
  EventLoopThread worker;
  std::atomic<int> destroyed{0}, executed{0};
  worker.start();
  assert(worker.post(makeProbe(worker, destroyed, executed)));
  worker.join();
  assert(executed == 1 && destroyed == 1);
  assert(!worker.post(makeProbe(worker, destroyed, executed)));
  assert(executed == 1 && destroyed == 2);
}

void testTaskAllocation(int failAt) {
  EventLoopThread worker;
  Gate gate;
  worker.start();
  assert(worker.post([&gate](EventLoop& loop) { gate.wait(loop); }));
  gate.entered.get_future().wait();
  std::atomic<int> destroyed{0}, executed{0};
  bool failed = false;
  int attempts = 0;
  // The deque allocates periodically. failAt=2 reaches its next allocation
  // after the shared slot and std::function storage have succeeded.
  while (!failed && attempts++ < 100) {
    auto task = makeProbe(worker, destroyed, executed);
    allocationCountdown = failAt;
    try {
      assert(worker.post(std::move(task)));
    } catch (const std::bad_alloc&) {
      failed = true;
    }
    allocationCountdown = -1;
  }
  assert(failed && destroyed == 1 && executed == 0);
  gate.release.set_value();
  worker.join();
  assert(destroyed == attempts);
  // Stop may discard or execute the queued tasks; no failed task executes.
  assert(executed < attempts);
}

void testTaskCapacity() {
  EventLoopThread worker;
  Gate gate;
  worker.start();
  assert(worker.post([&gate](EventLoop& loop) { gate.wait(loop); }));
  gate.entered.get_future().wait();
  for (std::size_t i = 1; i < EventLoop::kTaskCapacity; ++i)
    assert(worker.post([](EventLoop& loop) { ignoreTask(loop); }));
  std::atomic<int> destroyed{0}, executed{0};
  assert(!worker.post(makeProbe(worker, destroyed, executed)));
  assert(destroyed == 1 && executed == 0);
  gate.release.set_value();
  worker.join();
}

int sessionsCreated = 0;
int sessionsDestroyed = 0;
void observeSession(bool created, const void*) noexcept {
  if (created)
    ++sessionsCreated;
  else
    ++sessionsDestroyed;
}

hp::http::ResponseResult respond(const hp::http::HttpRequest&,
                                 hp::http::ConnectionPolicy policy) {
  const std::string body(512 * 1024, 'R');
  return {hp::http::makeResponse(
              hp::http::Status::kOk,
              {reinterpret_cast<const std::byte*>(body.data()), body.size()},
              "text/plain",
              false,
              policy),
          policy,
          {}};
}

void testHttpIsolationAndLifetime() {
  hp::app::setSessionEventCallback(observeSession);
  hp::app::HttpCallbackStats firstStats;
  {
    EventLoop loop;
    SocketFixture first(loop, 1), second(loop, 2);
    auto callback = hp::app::makeHttpCallback(
        [](const auto& request, auto policy) {
          return respond(request, policy);
        },
        &firstStats);
    // Copy before first invocation: each closure lazily owns a distinct
    // handler.
    auto secondCallback = callback;
    first.connection->setMessageCallback(std::move(callback));
    second.connection->setMessageCallback(std::move(secondCallback));
    first.connection->start();
    second.connection->start();
    assert(sessionsCreated == 0);
    first.write("GET /first HTTP/1.1\r\nHost: test\r\n");
    assert(sessionsCreated == 1 && firstStats.responses == 0);
    second.write("GET /second HTTP/1.1\r\nHost: test\r\n\r\n");
    assert(sessionsCreated == 2 && firstStats.responses == 1);
    assert(second.connection->pendingBytes() > 0);
    // Complete first independently, and queue another request behind blocked
    // IO.
    first.write("\r\n");
    assert(firstStats.responses == 2);
    second.write(
        "GET /third HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    std::string output;
    for (int i = 0; i < 1000 && firstStats.responses < 3; ++i) {
      output += second.readAll();
      first.readAll();
      loop.pollOnce(1);
    }
    assert(firstStats.responses == 3);
    // Message handler can die while write completion still owns its Session.
    second.connection->setMessageCallback({});
    assert(sessionsDestroyed == 0);
    for (int i = 0; i < 1000 && second.connection->pendingBytes(); ++i) {
      output += second.readAll();
      loop.pollOnce(1);
    }
    output += second.readAll();
    assert(second.connection->pendingBytes() == 0);
    assert(output.find("Connection: close") != std::string::npos);
    assert(sessionsDestroyed == 0);
    second.connection.reset();
    assert(sessionsDestroyed == 1);
  }
  assert(sessionsCreated == 2 && sessionsDestroyed == 2);
  hp::app::setSessionEventCallback(nullptr);
}
}  // namespace

int main() {
  testRegistration();
  testEcho(false);
  testEcho(true);
  testImmediateOwnerDestruction();
  testTaskSuccessAndStopped();
  testTaskAllocation(0);
  testTaskAllocation(1);
  testTaskAllocation(2);
  testTaskCapacity();
  testHttpIsolationAndLifetime();
  std::cout << "R6: registration, echo, task success/stop/capacity, "
               "slot/function/deque allocation failures, capture reentry, HTTP "
               "isolation/write lifetime PASS\n";
}
