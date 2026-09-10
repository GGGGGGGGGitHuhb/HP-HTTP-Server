#include "base/buffer.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace hp::base {
Buffer::Buffer(std::size_t limit) noexcept : limit_(limit) {}

Buffer::Buffer(Buffer&& other) noexcept
    : storage_(std::move(other.storage_)),
      capacity_(std::exchange(other.capacity_, 0)),
      limit_(other.limit_),
      read_(std::exchange(other.read_, 0)),
      write_(std::exchange(other.write_, 0)),
      prepared_(std::exchange(other.prepared_, 0)) {}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
  if (this != &other) {
    storage_ = std::move(other.storage_);
    capacity_ = std::exchange(other.capacity_, 0);
    limit_ = other.limit_;
    read_ = std::exchange(other.read_, 0);
    write_ = std::exchange(other.write_, 0);
    prepared_ = std::exchange(other.prepared_, 0);
  }
  return *this;
}

std::span<const std::byte> Buffer::readable_view() const noexcept {
  if (!storage_) return {};
  return {storage_.get() + read_, readable_bytes()};
}

std::span<std::byte> Buffer::prepare(std::size_t count) {
  const auto readable = readable_bytes();
  if (count > limit_ - readable)
    throw std::length_error("buffer limit exceeded");
  if (count > writable_bytes()) {
    const auto required = readable + count;
    if (required <= capacity_) {
      if (readable)
        std::memmove(storage_.get(), storage_.get() + read_, readable);
    } else {
      const auto doubled =
          capacity_ > limit_ - capacity_ ? limit_ : capacity_ * 2;
      const auto grown = std::max(required, doubled);
      // Allocate first: failure preserves the old bytes and all cursors.
      auto replacement =
          Storage(static_cast<std::byte*>(::operator new(grown)));
      if (readable)
        std::memcpy(replacement.get(), storage_.get() + read_, readable);
      storage_ = std::move(replacement);
      capacity_ = grown;
    }
    read_ = 0;
    write_ = readable;
  }
  prepared_ = count;
  if (!storage_) return {};
  return {storage_.get() + write_, count};
}

void Buffer::commit(std::size_t count) {
  if (count > prepared_)
    throw std::out_of_range("buffer commit exceeds prepared bytes");
  write_ += count;
  prepared_ = 0;
}

void Buffer::consume(std::size_t count) {
  if (count > readable_bytes())
    throw std::out_of_range("buffer consumption exceeds readable bytes");
  read_ += count;
  prepared_ = 0;
  if (read_ == write_) read_ = write_ = 0;
}

void Buffer::append(std::span<const std::byte> bytes) {
  auto tail = prepare(bytes.size());
  if (!bytes.empty()) std::memcpy(tail.data(), bytes.data(), bytes.size());
  commit(bytes.size());
}

void Buffer::reset() noexcept { read_ = write_ = prepared_ = 0; }

void Buffer::release_empty(std::size_t retain_limit) noexcept {
  if (readable_bytes() == 0 && capacity_ > retain_limit) {
    storage_.reset();
    capacity_ = 0;
    reset();
  }
}
}  // namespace hp::base
