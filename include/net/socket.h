#pragma once

#include <cstdint>

#include "base/non_copyable.h"

namespace hp::net {

class Socket final : private base::NonCopyable {
 public:
  Socket() noexcept = default;
  explicit Socket(int fd) noexcept;
  ~Socket() noexcept;

  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;

  [[nodiscard]] static Socket CreateTcp();

  [[nodiscard]] int fd() const noexcept;
  [[nodiscard]] bool valid() const noexcept;

  [[nodiscard]] int Release() noexcept;
  void Reset(int new_fd = -1) noexcept;

  void set_non_blocking();
  void set_reuse_address(bool enabled);

  void BindAny(std::uint16_t port);
  void Listen(int backlog);
  [[nodiscard]] Socket AcceptNonBlocking();

  [[nodiscard]] std::uint16_t local_port() const;
  [[nodiscard]] int socket_error() const;

 private:
  static constexpr int kInvalidFd = -1;

  int fd_{kInvalidFd};
};

}  // namespace hp::net
