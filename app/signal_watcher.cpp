#include "signal_watcher.h"

#include <pthread.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <system_error>

namespace hp::app {
SignalWatcher::SignalWatcher() {
  sigset_t signals;
  ::sigemptyset(&signals);
  ::sigaddset(&signals, SIGINT);
  ::sigaddset(&signals, SIGTERM);
  const int result = ::pthread_sigmask(SIG_BLOCK, &signals, &previous_);
  if (result)
    throw std::system_error(result, std::generic_category(),
                            "block shutdown signals");
  fd_ = ::signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC);
  if (fd_ < 0) {
    const int error = errno;
    ::pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
    throw std::system_error(error, std::generic_category(), "signalfd");
  }
}

SignalWatcher::~SignalWatcher() noexcept {
  ::close(fd_);
  ::pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
}

int SignalWatcher::next() {
  signalfd_siginfo signal{};
  for (;;) {
    const auto count = ::read(fd_, &signal, sizeof(signal));
    if (count == sizeof(signal)) return static_cast<int>(signal.ssi_signo);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 && errno == EAGAIN) return 0;
    if (count < 0)
      throw std::system_error(errno, std::generic_category(), "read signalfd");
    throw std::runtime_error("short signalfd read");
  }
}
}  // namespace hp::app
