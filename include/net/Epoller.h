#pragma once

#include <sys/epoll.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "base/NonCopyable.h"

namespace hp::net {

class Epoller final : private base::NonCopyable {
 public:
  explicit Epoller(std::size_t initialCapacity = 64);
  ~Epoller() noexcept;

  Epoller(Epoller&&) = delete;
  Epoller& operator=(Epoller&&) = delete;

  [[nodiscard]] int fd() const noexcept;

  void addDescriptor(int observedFd, std::uint32_t events, std::uint64_t token);
  void modifyDescriptor(int observedFd, std::uint32_t events, std::uint64_t token);
  void removeDescriptor(int observedFd) noexcept;
  [[nodiscard]] std::span<const epoll_event> waitForEvents(int timeoutMs);

 private:
  void controlDescriptor(int operation,
               int observedFd,
               std::uint32_t events,
               std::uint64_t token);

  int fd_{-1};
  std::vector<epoll_event> events_;
  std::size_t readyCount_{0};
};

}  // namespace hp::net
