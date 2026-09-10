#pragma once

#include <cstddef>
#include <limits>
#include <memory>
#include <span>

namespace hp::base {
// Single-owner contiguous storage. Views expire at the next mutation.
class Buffer {
 public:
  explicit Buffer(
      std::size_t limit = std::numeric_limits<std::size_t>::max()) noexcept;
  Buffer(Buffer&& other) noexcept;
  Buffer& operator=(Buffer&& other) noexcept;
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;

  std::span<const std::byte> readable_view() const noexcept;
  std::size_t readable_bytes() const noexcept { return write_ - read_; }
  std::size_t capacity() const noexcept { return capacity_; }
  std::size_t writable_bytes() const noexcept { return capacity_ - write_; }

  std::span<std::byte> prepare(std::size_t count);
  void commit(std::size_t count);
  void consume(std::size_t count);
  // Source must not alias this Buffer's storage.
  void append(std::span<const std::byte> bytes);
  void reset() noexcept;
  void release_empty(std::size_t retain_limit) noexcept;

 private:
  struct StorageDeleter {
    void operator()(std::byte* data) const noexcept { ::operator delete(data); }
  };

  using Storage = std::unique_ptr<std::byte[], StorageDeleter>;

  Storage storage_;
  std::size_t capacity_ = 0;
  std::size_t limit_;

  std::size_t read_ = 0;
  std::size_t write_ = 0;
  std::size_t prepared_ = 0;
};
}  // namespace hp::base
