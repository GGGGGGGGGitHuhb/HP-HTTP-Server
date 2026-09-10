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
  static void token(std::uint64_t v) {
    EventLoop::exchange_next_token_for_test(v);
  }
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
  Channel channel(loop, channel_fd, [](std::uint32_t) {});
  std::thread other([&] {
    try {
      loop.poll_once(0);
    } catch (const std::logic_error&) {
      ++rejected;
    }
    try {
      loop.set_after_dispatch({});
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
  throws([&] { loop.queue_in_loop({}); });
  check(loop.queue_in_loop([&] { ++n; }), "ready accepts");
  loop.request_stop();
  loop.request_stop();
  check(!loop.queue_in_loop([] {}), "prestop rejects");
  loop.loop();
  check(n == 1, "prestop drains");
  loop.poll_once(0);
  throws([&] { loop.loop(); });
  std::cout << "owner_ready: rejected=" << rejected << " executed=" << n
            << '\n';
}

void producers() {
  EventLoopThread worker;
  worker.start();
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
        while (!worker.post([&, p, i](EventLoop& loop) {
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
  worker.post([&](EventLoop& loop) {
    ++depth;
    max_depth = std::max(max_depth, depth);
    loop.queue_in_loop([&] {
      ++depth;
      max_depth = std::max(max_depth, depth);
      --depth;
      nested.set_value();
    });
    --depth;
  });
  check(nested.get_future().wait_for(3s) == std::future_status::ready,
        "nested deadline");
  worker.request_stop();
  worker.join();
  check(accepted == 4000 && executed == accepted && max_depth == 1,
        "task accounting/depth");
  for (auto n : seen) check(n == 1, "unique execution");
  std::cout << "producers: accepted=" << accepted << " executed=" << executed
            << " depth=" << max_depth << '\n';
}

void wake() {
  EventLoopThread worker;
  pid_t tid{};
  worker.start(
      [&](EventLoop&) { tid = static_cast<pid_t>(::syscall(SYS_gettid)); });
  blocked(tid);
  auto before = waits.load();
  auto start = std::chrono::steady_clock::now();
  std::promise<void> done;
  write_fault = 1;
  read_fault = 1;
  worker.post([&](EventLoop&) { done.set_value(); });
  check(done.get_future().wait_for(500ms) == std::future_status::ready,
        "eventfd post wake");
  blocked(tid);
  write_fault = 2;
  read_fault = 2;
  std::promise<void> again;
  worker.post([&](EventLoop&) { again.set_value(); });
  check(again.get_future().wait_for(500ms) == std::future_status::ready,
        "EAGAIN wake");
  blocked(tid);
  worker.request_stop();
  worker.join();
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
  check(!worker.post([](EventLoop&) {}), "before start rejects");
  worker.request_stop();
  worker.start();
  std::atomic<int> accepted{}, rejected{}, executed{};
  std::barrier gate(5);
  std::vector<std::thread> callers;
  for (int i = 0; i < 4; ++i)
    callers.emplace_back([&] {
      gate.arrive_and_wait();
      for (int j = 0; j < 1000; ++j)
        if (worker.post([&](EventLoop&) { ++executed; }))
          ++accepted;
        else
          ++rejected;
    });
  gate.arrive_and_wait();
  worker.request_stop();
  worker.request_stop();
  for (auto& t : callers) t.join();
  worker.join();
  worker.join();
  check(accepted == executed && accepted + rejected == 4000, "stop accounting");
  check(!worker.post([](EventLoop&) {}), "after join rejects");
  throws([&] { worker.start(); });
  std::cout << "stop_race: accepted=" << accepted << " executed=" << executed
            << " rejected=" << rejected << '\n';
}

void lifecycle() {
  auto fds = count("/proc/self/fd"), threads = count("/proc/self/task");
  int cleanup = 0;
  for (int i = 0; i < 100; ++i) {
    EventLoopThread worker;
    worker.start({}, [&](EventLoop& loop) {
      check(loop.is_in_loop_thread(), "cleanup owner");
      ++cleanup;
    });
    worker.post([](EventLoop&) {});
  }
  until([&] { return count("/proc/self/task") == threads; });
  check(count("/proc/self/fd") == fds && cleanup == 100, "resource baseline");
  std::cout << "lifecycle: cycles=" << cleanup << " fd=" << fds << '/'
            << count("/proc/self/fd") << " threads=" << threads << '/'
            << count("/proc/self/task") << '\n';
}

void failures() {
  int cleanup = 0;
  EventLoopThread init_fail;
  int partial_fd = -1;
  std::unique_ptr<Channel> partial_channel;
  auto before_fds = count("/proc/self/fd");
  throws([&] {
    init_fail.start(
        [&](EventLoop& loop) {
          partial_fd = ::eventfd(0, EFD_NONBLOCK);
          partial_channel =
              std::make_unique<Channel>(loop, partial_fd, [](std::uint32_t) {});
          partial_channel->set_interest(EPOLLIN);
          throw std::runtime_error("init");
        },
        [&](EventLoop& loop) {
          check(loop.is_in_loop_thread(), "partial cleanup owner");
          partial_channel->remove();
          partial_channel.reset();
          ::close(partial_fd);
          ++cleanup;
        });
  });
  init_fail.join();
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
  worker.start([&](EventLoop&) { owner = std::this_thread::get_id(); },
               [&](EventLoop&) { ++cleanup; });
  worker.post([&](EventLoop&) {
    ++executed;
    entered.set_value();
    gate.wait();
    throw std::runtime_error("task");
  });
  check(entered.get_future().wait_for(3s) == std::future_status::ready,
        "task entered");
  auto capture = std::shared_ptr<Capture>(new Capture{cancelled, owner});
  worker.post([capture](EventLoop&) {});
  capture.reset();
  release.set_value();
  throws([&] { worker.join(); });
  worker.join();
  check(executed == 1 && cancelled == 1 && cleanup == 2,
        "failure cancellation accounting");
  EventLoopThread self;
  self.start();
  self.post([&](EventLoop& loop) {
    throws([&] { self.join(); });
    loop.request_stop();
  });
  self.join();
  EventLoopThread bad_cleanup;
  bad_cleanup.start({},
                    [](EventLoop&) { throw std::runtime_error("cleanup"); });
  bad_cleanup.request_stop();
  throws([&] { bad_cleanup.join(); });
  {
    EventLoopThread unobserved;
    unobserved.start([](EventLoop& loop) {
      loop.queue_in_loop([] {
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
    worker.start([&](EventLoop&) {
      init.set_value();
      gate.wait();
    });
    returned = true;
  });
  check(init.get_future().wait_for(3s) == std::future_status::ready,
        "init handshake");
  check(!returned, "start waits for init");
  worker.request_stop();
  release.set_value();
  control.join();
  worker.join();
  check(!worker.post([](EventLoop&) {}), "starting stop remembered");
  std::cout << "start_stop: readiness_and_stop_intent=observed\n";
}

void syscall_failures() {
  auto fds = count("/proc/self/fd");
  create_error = 1;
  throws([] {
    EventLoopThread w;
    w.start();
  });
  register_error = 1;
  throws([] {
    EventLoopThread w;
    w.start();
  });
  for (int fault : {3, 4}) {
    for (bool write : {false, true}) {
      EventLoopThread worker;
      pid_t tid{};
      worker.start(
          [&](EventLoop&) { tid = static_cast<pid_t>(::syscall(SYS_gettid)); });
      blocked(tid);
      if (write)
        write_fault = fault;
      else
        read_fault = fault;
      auto start = std::chrono::steady_clock::now();
      check(worker.post([](EventLoop&) {}), "wake failure still accepted");
      throws([&] { worker.join(); });
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

  worker.start(
      [&](EventLoop& loop) {
        fd = ::eventfd(1, EFD_NONBLOCK);
        channel = std::make_unique<Channel>(
            loop, fd, [](std::uint32_t) { throw std::runtime_error("IO"); });
        channel->set_interest(EPOLLIN);
        loop.set_after_dispatch([&] {
          channel->remove();
          ++dispatch_cleanup;
        });
        auto capture = std::shared_ptr<Capture>(
            new Capture{cancelled, std::this_thread::get_id()});
        loop.queue_in_loop([capture] {});
      },
      [&](EventLoop&) {
        ++cleanup;
        channel->remove();
        channel.reset();
        ::close(fd);
      });
  // Successful init is distinct from a subsequent immediate IO failure.
  throws([&] { worker.join(); });
  check(dispatch_cleanup == 1 && cleanup == 1 && cancelled == 1,
        "IO exceptional cleanup");
  EventLoop loop;
  fd = ::eventfd(1, EFD_NONBLOCK);
  int io = 0, tasks = 0;
  Channel readable(loop, fd, [&](std::uint32_t) { ++io; });
  readable.set_interest(EPOLLIN);
  std::function<void()> chain;
  chain = [&] {
    if (++tasks < 5) loop.queue_in_loop(chain);
  };
  loop.queue_in_loop(chain);
  loop.poll_once(0);
  check(tasks == 1, "manual snapshot only");
  for (int i = 0; i < 4; ++i) loop.poll_once(0);
  check(tasks == 5 && io == 5, "IO advances with nested tasks");
  readable.remove();
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

  worker.start([&](EventLoop& loop) {
    loop.queue_in_loop([&] {
      ++executed;
      auto nested = std::shared_ptr<Capture>(
          new Capture{cancelled, std::this_thread::get_id()});
      loop.queue_in_loop([nested] {});
      throw std::runtime_error("snapshot");
    });
    auto capture = std::shared_ptr<Capture>(
        new Capture{cancelled, std::this_thread::get_id()});
    loop.queue_in_loop([capture] {});
  });
  throws([&] { worker.join(); });
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
      auto channel = std::make_unique<Channel>(*loop, fd, [](std::uint32_t) {});
      std::thread wrong([&] {
        if (operation == 0) channel->remove();
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
    Channel channel(loop, fd, [](std::uint32_t) {});
    gate.arrive_and_wait();
    for (int i = 0; i < 1000; ++i) {
      channel.set_interest(EPOLLIN);
      channel.remove();
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
    Channel channel(loop, fd, [](std::uint32_t) {});
    EventLoopTestAccess::token(std::numeric_limits<std::uint64_t>::max());
    channel.set_interest(EPOLLIN);
    check(channel.token() == std::numeric_limits<std::uint64_t>::max(),
          "last token");
    channel.remove();
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
}  // namespace

int main() {
  try {
    owner_and_ready();
    producers();
    wake();
    stop_race();
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
