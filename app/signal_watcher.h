#pragma once
#include <signal.h>

#include "base/non_copyable.h"

namespace hp::app {
// Construct before workers; destroy after their join so they inherit a blocked
// mask.
class SignalWatcher final : private base::NonCopyable {
 public:
  SignalWatcher();
  ~SignalWatcher() noexcept;

  int fd() const noexcept { return fd_; }

  int next();

 private:
  sigset_t previous_{};
  int fd_{-1};
};
}  // namespace hp::app
