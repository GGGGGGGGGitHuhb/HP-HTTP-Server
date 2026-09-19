#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

#include "base/buffer.h"
#include "base/file_region.h"
#include "base/non_copyable.h"
#include "net/socket.h"

namespace hp::net {
struct ReadResult {
  std::size_t bytes_read{0};
  bool would_block{false};
  bool peer_closed{false};
  int error_number{0};
};

struct WriteResult {
  std::size_t bytes_written{0};
  bool file_transfer{false};
  bool would_block{false};
  int error_number{0};
};

struct ConnectionEventResult {
  std::size_t bytes_read{0};
  std::size_t bytes_written{0};
  bool write_would_block{false};
  bool socket_error_observed{false};
  int socket_error{0};
  int socket_error_query_error{0};
  int read_error{0};
  int write_error{0};
  bool close_requested{false};
};

class ConnectionIo final : private base::NonCopyable {
 public:
  static constexpr std::size_t kOutputLimit = 9U * 1024U * 1024U;
  static constexpr std::size_t kFileWriteBudget = 256U * 1024U;
  static constexpr std::size_t kFileCallBudget = 16;

  static bool OutputFits(std::size_t pending, std::size_t incoming) noexcept;

  explicit ConnectionIo(Socket socket,
                        std::size_t max_input_bytes = 0) noexcept;
  ConnectionIo(ConnectionIo&&) noexcept = default;
  ConnectionIo& operator=(ConnectionIo&&) noexcept = default;

  [[nodiscard]] int fd() const noexcept;

  [[nodiscard]] ReadResult ReadAvailable();
  [[nodiscard]] WriteResult WriteAvailable();
  [[nodiscard]] ReadResult ReadOnce();

  [[nodiscard]] int socket_error() const;

  [[nodiscard]] std::span<const std::byte> input_view() const noexcept;
  void Consume(std::size_t count);

  void QueueOutput(std::span<const std::byte> bytes);
  void QueueFile(std::span<const std::byte> header, base::FileRegion file);

  void MarkPeerHalfClosed() noexcept;
  [[nodiscard]] bool peer_half_closed() const noexcept;
  [[nodiscard]] bool has_pending_output() const noexcept;
  [[nodiscard]] bool accepts_input() const noexcept;
  [[nodiscard]] std::size_t pending_bytes() const noexcept;
  [[nodiscard]] bool ready_to_close() const noexcept;

 private:
  friend struct ConnectionIoTestAccess;

  Socket socket_;

  std::size_t max_input_bytes_{0};
  base::Buffer input_;

  base::Buffer output_{kOutputLimit};
  std::optional<base::FileRegion> file_;

  bool peer_half_closed_{false};
};

}  // namespace hp::net
