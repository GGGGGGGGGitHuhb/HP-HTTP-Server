#pragma once
#include <cstdint>
#include <functional>

#include "base/NonCopyable.h"

namespace hp::net {
class EventLoop;

// 不持有所有权的观察对象。在析构前移除，并保持存活直到回调
// 返回。所有操作必须在事件循环线程上执行，包括事件循环
// 启动前的操作。
class Channel final : private base::NonCopyable {
 public:
  using EventCallback = std::function<void(std::uint32_t)>;

  Channel(EventLoop& ownerEventLoop, int fd);
  void registerEventCallback(EventCallback eventCallback);
  ~Channel() noexcept;

  void setInterest(std::uint32_t events);
  void removeChannel() noexcept;

  [[nodiscard]] int fd() const noexcept { return fd_; }
  [[nodiscard]] std::uint32_t interest() const noexcept { return interest_; }
  [[nodiscard]] std::uint32_t revents() const noexcept { return revents_; }
  [[nodiscard]] std::uint64_t token() const noexcept { return token_; }
  [[nodiscard]] bool registered() const noexcept { return token_ != 0; }

 private:
  friend class EventLoop;

  void setEventCallback(EventCallback eventCallback) { eventCallback_ = std::move(eventCallback); }

  EventLoop& ownerEventLoop_;
  const int fd_;
  // `listenerChannel_` 绑定 `Acceptor::handleListenerEvent()`
  // `connectionChannel_` 绑定 `TcpConnection::handleConnectionEvent()`
  // `wakeChannel_` 绑定 `EventLoop::handleWakeupEvent()`
  EventCallback eventCallback_;

  std::uint32_t interest_{0};
  std::uint32_t revents_{0};
  std::uint64_t token_{0};
};
}  // namespace hp::net
