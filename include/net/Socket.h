#pragma once

#include <cstdint>

#include "base/NonCopyable.h"

namespace hp::net {

class Socket final : private base::NonCopyable {
 public:
  Socket() noexcept = default;
  explicit Socket(int fd) noexcept;
  ~Socket() noexcept;

  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;

  [[nodiscard]] static Socket createTcp();

  [[nodiscard]] int fd() const noexcept;
  [[nodiscard]] bool valid() const noexcept;

  [[nodiscard]] int release() noexcept;
  void reset(int newFd = -1) noexcept;

  void setNonBlocking();
  void setReuseAddress(bool enabled);
  void setTcpNoDelay(bool enabled);

  void bindAny(std::uint16_t port);
  void listen(int backlog);
  [[nodiscard]] Socket acceptNonBlocking();

  [[nodiscard]] std::uint16_t localPort() const;
  [[nodiscard]] int socketError() const;

 private:
  static constexpr int kInvalidFd = -1;

  int fd_{kInvalidFd};
};

}  // namespace hp::net
