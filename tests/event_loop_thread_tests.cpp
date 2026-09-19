#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <climits>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>

#include "net/channel.h"
#include "net/event_loop_thread.h"

using namespace hp::net;
using namespace std::chrono_literals;

namespace hp::net {
struct EventLoopTestAccess {
  static void token(std::uint64_t v) { EventLoop::ExchangeNextTokenForTest(v); }
};
}  // namespace hp::net

namespace {
std::atomic<int> create_error{}, register_error{}, read_fault{}, write_fault{};
std::atomic<int> reads{}, writes{}, read_eintr{}, write_eintr{}, read_eagain{},
    write_eagain{}, waits{};

void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

template <class F>
void throws(F f) {
  bool caught = false;
  try {
    f();
  } catch (const std::exception&) {
    caught = true;
  }
  check(caught, "expected exception");
}

template <class F>
void until(F f) {
  auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!f()) {
    check(std::chrono::steady_clock::now() < deadline, "deadline");
    std::this_thread::yield();
  }
}

void blocked(pid_t tid) {
  until([&] {
    std::ifstream in("/proc/self/task/" + std::to_string(tid) + "/syscall");
    long call = -1;
    in >> call;
    return call == SYS_epoll_wait;
  });
}

std::size_t count(const char* path) {
  return static_cast<std::size_t>(
      std::distance(std::filesystem::directory_iterator(path), {}));
}
}  // namespace

extern "C" {
int __real_eventfd(unsigned int, int);

int __wrap_eventfd(unsigned int value, int flags) {
  if (create_error.exchange(0)) {
    errno = EMFILE;
    return -1;
  }
  return __real_eventfd(value, flags);
}

int __real_epoll_ctl(int, int, int, epoll_event*);

int __wrap_epoll_ctl(int fd, int op, int observed, epoll_event* event) {
  if (op == EPOLL_CTL_ADD && register_error.exchange(0)) {
    errno = ENOSPC;
    return -1;
  }
  return __real_epoll_ctl(fd, op, observed, event);
}

int __real_epoll_wait(int, epoll_event*, int, int);

int __wrap_epoll_wait(int fd, epoll_event* event, int max, int timeout) {
  ++waits;
  return __real_epoll_wait(fd, event, max, timeout);
}

ssize_t __real_read(int, void*, size_t);

ssize_t __wrap_read(int fd, void* data, size_t size) {
  if (size == 8) {
    ++reads;
    const int fault = read_fault.exchange(0);
    if (fault == 1) {
      ++read_eintr;
      errno = EINTR;
      return -1;
    }
    if (fault == 2) {
      ++read_eagain;
      errno = EAGAIN;
      return -1;
    }
    if (fault == 3) {
      errno = EIO;
      return -1;
    }
    if (fault == 4) return 4;
  }
  auto result = __real_read(fd, data, size);
  if (size == 8 && result < 0 && errno == EAGAIN) ++read_eagain;
  return result;
}

ssize_t __real_write(int, const void*, size_t);

ssize_t __wrap_write(int fd, const void* data, size_t size) {
  if (size == 8) {
    ++writes;
    const int fault = write_fault.exchange(0);
    if (fault == 1) {
      ++write_eintr;
      errno = EINTR;
      return -1;
    }
    // Leave a real pending notification to reproduce saturated eventfd.
    if (fault == 2) {
      __real_write(fd, data, size);
      ++write_eagain;
      errno = EAGAIN;
      return -1;
    }
    if (fault == 3) {
      errno = EIO;
      return -1;
    }
    if (fault == 4) return 4;
  }
  return __real_write(fd, data, size);
}
}

namespace {
void owner_and_ready() {
  EventLoop loop;
  std::atomic<int> rejected{};
  int channel_fd = ::eventfd(0, EFD_NONBLOCK);
  Channel channel(loop, channel_fd);
  struct HandleConnectionEventObserver1 {
    void HandleConnectionEvent(std::uint32_t) {}
  };
  channel.set_HandleConnectionEvent_callback(
      std::bind_front(&HandleConnectionEventObserver1::HandleConnectionEvent,
                      HandleConnectionEventObserver1{}));
  std::thread other([&] {
    try {
      loop.PollOnce(0);
    } catch (const std::logic_error&) {
      ++rejected;
    }
    try {
      loop.set_DrainClosedConnections_callback({});
    } catch (const std::logic_error&) {
      ++rejected;
    }
    try {
      channel.set_interest(EPOLLIN);
    } catch (const std::logic_error&) {
      ++rejected;
    }
  });
  other.join();
  check(rejected == 3, "owner rejection");
  ::close(channel_fd);
  int n = 0;
  throws([&] { loop.QueueInLoop({}); });
  check(loop.QueueInLoop([&] { ++n; }), "ready accepts");
  loop.RequestStop();
  loop.RequestStop();
  check(!loop.QueueInLoop([] {}), "prestop rejects");
  loop.Loop();
  check(n == 1, "prestop drains");
  loop.PollOnce(0);
  throws([&] { loop.Loop(); });
  std::cout << "owner_ready: rejected=" << rejected << " executed=" << n
            << '\n';
}

void producers() {
  EventLoopThread worker;
  worker.Start();
  std::barrier gate(5);
  std::vector<std::thread> producers;
  std::vector<int> seen(4000);
  int next[4]{};
  int executed = 0, depth = 0, max_depth = 0;
  std::atomic<int> accepted{};
  for (int p = 0; p < 4; ++p)
    producers.emplace_back([&, p] {
      gate.arrive_and_wait();
      for (int i = 0; i < 1000; ++i) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!worker.Post([&, p, i](EventLoop& loop) {
          check(loop.is_in_loop_thread(), "owner execution");
          check(next[p]++ == i, "producer order");
          ++seen[p * 1000 + i];
          ++executed;
        })) {
          check(std::chrono::steady_clock::now() < deadline,
                "bounded task retry deadline");
          std::this_thread::yield();
        }
        ++accepted;
      }
    });
  gate.arrive_and_wait();
  for (auto& t : producers) t.join();
  std::promise<void> nested;
  worker.Post([&](EventLoop& loop) {
    ++depth;
    max_depth = std::max(max_depth, depth);
    loop.QueueInLoop([&] {
      ++depth;
      max_depth = std::max(max_depth, depth);
      --depth;
      nested.set_value();
    });
    --depth;
  });
  check(nested.get_future().wait_for(3s) == std::future_status::ready,
        "nested deadline");
  worker.RequestStop();
  worker.Join();
  check(accepted == 4000 && executed == accepted && max_depth == 1,
        "task accounting/depth");
  for (auto n : seen) check(n == 1, "unique execution");
  std::cout << "producers: accepted=" << accepted << " executed=" << executed
            << " depth=" << max_depth << '\n';
}

void wake() {
  EventLoopThread worker;
  pid_t tid{};
  worker.Start(
      [&](EventLoop&) { tid = static_cast<pid_t>(::syscall(SYS_gettid)); });
  blocked(tid);
  auto before = waits.load();
  auto start = std::chrono::steady_clock::now();
  std::promise<void> done;
  write_fault = 1;
  read_fault = 1;
  worker.Post([&](EventLoop&) { done.set_value(); });
  check(done.get_future().wait_for(500ms) == std::future_status::ready,
        "eventfd post wake");
  blocked(tid);
  write_fault = 2;
  read_fault = 2;
  std::promise<void> again;
  worker.Post([&](EventLoop&) { again.set_value(); });
  check(again.get_future().wait_for(500ms) == std::future_status::ready,
        "EAGAIN wake");
  blocked(tid);
  worker.RequestStop();
  worker.Join();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count();
  check(elapsed < 500 && waits - before < 10,
        "healthy wake before fallback/no spin");
  check(read_eintr && write_eintr && read_eagain && write_eagain,
        "fault coverage");
  std::cout << "wake: real_epoll_wait_handshake=3 elapsed_ms=" << elapsed
            << " waits=" << waits - before << " reads=" << reads
            << " writes=" << writes << " EINTR=" << read_eintr << '/'
            << write_eintr << " EAGAIN=" << read_eagain << '/' << write_eagain
            << '\n';
}

void stop_race() {
  EventLoopThread worker;
  check(!worker.Post([](EventLoop&) {}), "before start rejects");
  worker.RequestStop();
  worker.Start();
  std::atomic<int> accepted{}, rejected{}, executed{};
  std::barrier gate(5);
  std::vector<std::thread> callers;
  for (int i = 0; i < 4; ++i)
    callers.emplace_back([&] {
      gate.arrive_and_wait();
      for (int j = 0; j < 1000; ++j)
        if (worker.Post([&](EventLoop&) { ++executed; }))
          ++accepted;
        else
          ++rejected;
    });
  gate.arrive_and_wait();
  worker.RequestStop();
  worker.RequestStop();
  for (auto& t : callers) t.join();
  worker.Join();
  worker.Join();
  check(accepted == executed && accepted + rejected == 4000, "stop accounting");
  check(!worker.Post([](EventLoop&) {}), "after join rejects");
  throws([&] { worker.Start(); });
  std::cout << "stop_race: accepted=" << accepted << " executed=" << executed
            << " rejected=" << rejected << '\n';
}

void lifecycle() {
  auto fds = count("/proc/self/fd"), threads = count("/proc/self/task");
  int cleanup = 0;
  for (int i = 0; i < 100; ++i) {
    EventLoopThread worker;
    worker.Start({}, [&](EventLoop& loop) {
      check(loop.is_in_loop_thread(), "cleanup owner");
      ++cleanup;
    });
    worker.Post([](EventLoop&) {});
  }
  until([&] { return count("/proc/self/task") == threads; });
  check(count("/proc/self/fd") == fds && cleanup == 100, "resource baseline");
  std::cout << "lifecycle: cycles=" << cleanup << " fd=" << fds << '/'
            << count("/proc/self/fd") << " threads=" << threads << '/'
            << count("/proc/self/task") << '\n';
}

struct PassiveChannelObserver {
  static void HandleConnectionEvent(std::uint32_t) {}
};

struct FailingChannelObserver {
  static void HandleConnectionEvent(std::uint32_t) {
    throw std::runtime_error("IO");
  }
};

void failures() {
  int cleanup = 0;
  EventLoopThread init_fail;
  int partial_fd = -1;
  std::unique_ptr<Channel> partial_channel;
  auto before_fds = count("/proc/self/fd");
  throws([&] {
    init_fail.Start(
        [&](EventLoop& loop) {
          partial_fd = ::eventfd(0, EFD_NONBLOCK);
          partial_channel = std::make_unique<Channel>(loop, partial_fd);
          partial_channel->set_HandleConnectionEvent_callback(
              &PassiveChannelObserver::HandleConnectionEvent);
          partial_channel->set_interest(EPOLLIN);
          throw std::runtime_error("init");
        },
        [&](EventLoop& loop) {
          check(loop.is_in_loop_thread(), "partial cleanup owner");
          partial_channel->Remove();
          partial_channel.reset();
          ::close(partial_fd);
          ++cleanup;
        });
  });
  init_fail.Join();
  check(cleanup == 1 && count("/proc/self/fd") == before_fds,
        "partial init cleanup once");
  EventLoopThread worker;
  std::promise<void> entered, release;
  auto gate = release.get_future().share();
  int executed = 0, cancelled = 0;

  struct Capture {
    int& count;
    std::thread::id owner;

    ~Capture() {
      if (std::this_thread::get_id() != owner) std::abort();
      ++count;
    }
  };

  std::thread::id owner;
  worker.Start([&](EventLoop&) { owner = std::this_thread::get_id(); },
               [&](EventLoop&) { ++cleanup; });
  worker.Post([&](EventLoop&) {
    ++executed;
    entered.set_value();
    gate.wait();
    throw std::runtime_error("task");
  });
  check(entered.get_future().wait_for(3s) == std::future_status::ready,
        "task entered");
  auto capture = std::shared_ptr<Capture>(new Capture{cancelled, owner});
  worker.Post([capture](EventLoop&) {});
  capture.reset();
  release.set_value();
  throws([&] { worker.Join(); });
  worker.Join();
  check(executed == 1 && cancelled == 1 && cleanup == 2,
        "failure cancellation accounting");
  EventLoopThread self;
  self.Start();
  self.Post([&](EventLoop& loop) {
    throws([&] { self.Join(); });
    loop.RequestStop();
  });
  self.Join();
  EventLoopThread bad_cleanup;
  bad_cleanup.Start({},
                    [](EventLoop&) { throw std::runtime_error("cleanup"); });
  bad_cleanup.RequestStop();
  throws([&] { bad_cleanup.Join(); });
  {
    EventLoopThread unobserved;
    unobserved.Start([](EventLoop& loop) {
      loop.QueueInLoop([] {
        throw std::runtime_error("expected unobserved worker failure");
      });
    });
  }
  std::cout << "failures: accepted=2 executed=" << executed
            << " cancelled=" << cancelled << " cleanup=" << cleanup << '\n';
}

void start_stop() {
  EventLoopThread worker;
  std::promise<void> init, release;
  auto gate = release.get_future().share();
  std::atomic<bool> returned{};
  std::thread control([&] {
    worker.Start([&](EventLoop&) {
      init.set_value();
      gate.wait();
    });
    returned = true;
  });
  check(init.get_future().wait_for(3s) == std::future_status::ready,
        "init handshake");
  check(!returned, "start waits for init");
  worker.RequestStop();
  release.set_value();
  control.join();
  worker.Join();
  check(!worker.Post([](EventLoop&) {}), "starting stop remembered");
  std::cout << "start_stop: readiness_and_stop_intent=observed\n";
}

void syscall_failures() {
  auto fds = count("/proc/self/fd");
  create_error = 1;
  throws([] {
    EventLoopThread w;
    w.Start();
  });
  register_error = 1;
  throws([] {
    EventLoopThread w;
    w.Start();
  });
  for (int fault : {3, 4}) {
    for (bool write : {false, true}) {
      EventLoopThread worker;
      pid_t tid{};
      worker.Start(
          [&](EventLoop&) { tid = static_cast<pid_t>(::syscall(SYS_gettid)); });
      blocked(tid);
      if (write)
        write_fault = fault;
      else
        read_fault = fault;
      auto start = std::chrono::steady_clock::now();
      check(worker.Post([](EventLoop&) {}), "wake failure still accepted");
      throws([&] { worker.Join(); });
      check(std::chrono::steady_clock::now() - start < 2s,
            "broken wake bounded");
    }
  }
  check(count("/proc/self/fd") == fds, "failure rollback fd");
  std::cout << "syscall_failures: create=1 register=1 permanent=2 short=2 "
               "rollback=verified\n";
}

void io_failure_and_fairness() {
  EventLoopThread worker;
  std::unique_ptr<Channel> channel;
  int fd = -1, dispatch_cleanup = 0, cleanup = 0, cancelled = 0;

  struct Capture {
    int& cancelled;
    std::thread::id owner;

    ~Capture() {
      check(std::this_thread::get_id() == owner, "IO cancel owner");
      ++cancelled;
    }
  };

  struct ChannelCleanupTarget {
    std::unique_ptr<Channel>& channel;
    int& dispatch_cleanup;
    void DrainClosedConnections() {
      channel->Remove();
      ++dispatch_cleanup;
    }
  } cleanup_target{channel, dispatch_cleanup};

  worker.Start(
      [&](EventLoop& loop) {
        fd = ::eventfd(1, EFD_NONBLOCK);
        channel = std::make_unique<Channel>(loop, fd);
        channel->set_HandleConnectionEvent_callback(
            &FailingChannelObserver::HandleConnectionEvent);
        channel->set_interest(EPOLLIN);
        loop.set_DrainClosedConnections_callback(
            std::bind_front(&ChannelCleanupTarget::DrainClosedConnections,
                            &cleanup_target));
        auto capture = std::shared_ptr<Capture>(
            new Capture{cancelled, std::this_thread::get_id()});
        loop.QueueInLoop([capture] {});
      },
      [&](EventLoop&) {
        ++cleanup;
        channel->Remove();
        channel.reset();
        ::close(fd);
      });
  // Successful init is distinct from a subsequent immediate IO failure.
  throws([&] { worker.Join(); });
  check(dispatch_cleanup == 1 && cleanup == 1 && cancelled == 1,
        "IO exceptional cleanup");
  EventLoop loop;
  fd = ::eventfd(1, EFD_NONBLOCK);
  int io = 0, tasks = 0;
  Channel readable(loop, fd);
  using HandleConnectionEventObserver2State0 = decltype((io));
  struct HandleConnectionEventObserver2 {
    HandleConnectionEventObserver2State0 io;
    void HandleConnectionEvent(std::uint32_t) { ++io; }
  };
  readable.set_HandleConnectionEvent_callback(
      std::bind_front(&HandleConnectionEventObserver2::HandleConnectionEvent,
                      HandleConnectionEventObserver2{io}));
  readable.set_interest(EPOLLIN);
  std::function<void()> chain;
  chain = [&] {
    if (++tasks < 5) loop.QueueInLoop(chain);
  };
  loop.QueueInLoop(chain);
  loop.PollOnce(0);
  check(tasks == 1, "manual snapshot only");
  for (int i = 0; i < 4; ++i) loop.PollOnce(0);
  check(tasks == 5 && io == 5, "IO advances with nested tasks");
  readable.Remove();
  ::close(fd);
  std::cout << "io_failure: task_accepted=1 executed=0 cancelled=" << cancelled
            << " after_dispatch=" << dispatch_cleanup << " cleanup=" << cleanup
            << " fairness_io=" << io << " tasks=" << tasks << '\n';
}

void snapshot_cancellation() {
  EventLoopThread worker;
  int executed = 0, cancelled = 0;

  struct Capture {
    int& count;
    std::thread::id owner;

    ~Capture() {
      check(std::this_thread::get_id() == owner, "snapshot cancel owner");
      ++count;
    }
  };

  worker.Start([&](EventLoop& loop) {
    loop.QueueInLoop([&] {
      ++executed;
      auto nested = std::shared_ptr<Capture>(
          new Capture{cancelled, std::this_thread::get_id()});
      loop.QueueInLoop([nested] {});
      throw std::runtime_error("snapshot");
    });
    auto capture = std::shared_ptr<Capture>(
        new Capture{cancelled, std::this_thread::get_id()});
    loop.QueueInLoop([capture] {});
  });
  throws([&] { worker.Join(); });
  check(executed == 1 && cancelled == 2, "snapshot and pending cancelled");
  std::cout << "snapshot_failure: accepted=3 executed=" << executed
            << " cancelled=" << cancelled << '\n';
}

void owner_assertions() {
  for (int operation = 0; operation < 3; ++operation) {
    auto child = ::fork();
    check(child >= 0, "owner probe fork");
    if (!child) {
      rlimit limit{0, 0};
      ::setrlimit(RLIMIT_CORE, &limit);
      auto loop = std::make_unique<EventLoop>();
      int fd = ::eventfd(0, EFD_NONBLOCK);
      auto channel = std::make_unique<Channel>(*loop, fd);
      channel->set_HandleConnectionEvent_callback(
          &PassiveChannelObserver::HandleConnectionEvent);
      std::thread wrong([&] {
        if (operation == 0) channel->Remove();
        if (operation == 1) channel.reset();
        if (operation == 2) loop.reset();
      });
      wrong.join();
      _exit(1);
    }
    int status{};
    check(::waitpid(child, &status, 0) == child && WIFSIGNALED(status) &&
              WTERMSIG(status) == SIGABRT,
          "noexcept owner assertion");
  }
  std::cout << "owner_assertions: isolated_SIGABRT=3\n";
}

void tokens() {
  std::barrier gate(2);
  std::atomic<int> registrations{};
  auto run = [&] {
    EventLoop loop;
    int fd = ::eventfd(0, EFD_NONBLOCK);
    Channel channel(loop, fd);
    struct HandleConnectionEventObserver3 {
      void HandleConnectionEvent(std::uint32_t) {}
    };
    channel.set_HandleConnectionEvent_callback(
        std::bind_front(&HandleConnectionEventObserver3::HandleConnectionEvent,
                        HandleConnectionEventObserver3{}));
    gate.arrive_and_wait();
    for (int i = 0; i < 1000; ++i) {
      channel.set_interest(EPOLLIN);
      channel.Remove();
      ++registrations;
    }
    ::close(fd);
  };
  std::thread a(run), b(run);
  a.join();
  b.join();
  pid_t child = ::fork();
  check(child >= 0, "fork");
  if (!child) {
    EventLoop loop;
    int fd = ::eventfd(0, EFD_NONBLOCK);
    Channel channel(loop, fd);
    struct HandleConnectionEventObserver4 {
      void HandleConnectionEvent(std::uint32_t) {}
    };
    channel.set_HandleConnectionEvent_callback(
        std::bind_front(&HandleConnectionEventObserver4::HandleConnectionEvent,
                        HandleConnectionEventObserver4{}));
    EventLoopTestAccess::token(std::numeric_limits<std::uint64_t>::max());
    channel.set_interest(EPOLLIN);
    check(channel.token() == std::numeric_limits<std::uint64_t>::max(),
          "last token");
    channel.Remove();
    throws([&] { channel.set_interest(EPOLLIN); });
    throws([&] { channel.set_interest(EPOLLIN); });
    ::close(fd);
    _exit(0);
  }
  int status{};
  check(::waitpid(child, &status, 0) == child && WIFEXITED(status) &&
            WEXITSTATUS(status) == 0,
        "token exhaustion child");
  std::cout << "tokens: concurrent_registrations=" << registrations
            << " exhaustion_latched=2\n";
}
void named_startup_first_error() {
  struct StartupTarget {
    int cleanup{0};
    std::thread::id init_owner;
    void Initialize(EventLoop& loop) {
      init_owner = std::this_thread::get_id();
      check(loop.is_in_loop_thread(), "named init owns loop");
      throw std::runtime_error("named init first");
    }
    void Cleanup(EventLoop& loop) {
      check(
          loop.is_in_loop_thread() && init_owner == std::this_thread::get_id(),
          "named partial cleanup same owner");
      ++cleanup;
      throw std::runtime_error("named cleanup second");
    }
  } target;
  EventLoopThread worker;
  bool caught = false;
  try {
    worker.Start(std::bind_front(&StartupTarget::Initialize, &target),
                 std::bind_front(&StartupTarget::Cleanup, &target));
  } catch (const std::runtime_error& error) {
    caught = std::string(error.what()) == "named init first";
  }
  worker.Join();
  check(caught && target.cleanup == 1,
        "thread entry preserves first startup error once");
}

}  // namespace

int main() {
  try {
    owner_and_ready();
    producers();
    wake();
    stop_race();
    named_startup_first_error();
    lifecycle();
    failures();
    start_stop();
    syscall_failures();
    io_failure_and_fairness();
    snapshot_cancellation();
    owner_assertions();
    tokens();
    std::cout << "event_loop_thread_tests: PASS\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
