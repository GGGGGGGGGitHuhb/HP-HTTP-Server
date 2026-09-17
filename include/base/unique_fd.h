#pragma once
#include <unistd.h>

#include <utility>

namespace hp::base {
class UniqueFd final {
 public:
  explicit UniqueFd(int fd = -1) noexcept : fd_(fd) {}

  ~UniqueFd() noexcept { Reset(); }

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  UniqueFd(UniqueFd&& other) noexcept : fd_(other.Release()) {}

  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) Reset(other.Release());
    return *this;
  }

  [[nodiscard]] int get() const noexcept { return fd_; }

  [[nodiscard]] int Release() noexcept { return std::exchange(fd_, -1); }

  void Reset(int fd = -1) noexcept {
    const int previous = std::exchange(fd_, fd);
    if (previous >= 0) ::close(previous);
  }

 private:
  int fd_;
};
}  // namespace hp::base
