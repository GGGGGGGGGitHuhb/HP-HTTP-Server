#pragma once
#include <signal.h>

#include "base/NonCopyable.h"

namespace hp::app {
// 在工作线程创建前构造，让它们继承屏蔽的信号掩码；在它们 join 后
// 销毁。
class SignalWatcher final : private base::NonCopyable {
 public:
  SignalWatcher();
  ~SignalWatcher() noexcept;

  int fd() const noexcept { return fd_; }

  int readNextSignal();

 private:
  sigset_t previous_{};
  int fd_{-1};
};
}  // namespace hp::app
