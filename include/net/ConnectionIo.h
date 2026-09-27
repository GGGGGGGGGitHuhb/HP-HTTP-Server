#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

#include "base/Buffer.h"
#include "base/FileRegion.h"
#include "base/NonCopyable.h"
#include "net/Socket.h"

namespace hp::net {
struct ReadResult {
  std::size_t bytesRead{0};
  bool wouldBlock{false};
  bool peerClosed{false};
  int errorNumber{0};
};

struct WriteResult {
  std::size_t bytesWritten{0};
  bool fileTransfer{false};
  bool wouldBlock{false};
  int errorNumber{0};
};

struct ConnectionEventResult {
  std::size_t bytesRead{0};
  std::size_t bytesWritten{0};
  bool writeWouldBlock{false};
  bool socketErrorObserved{false};
  int socketError{0};
  int socketErrorQueryError{0};
  int readError{0};
  int writeError{0};
  bool closeRequested{false};
};

class ConnectionIo final : private base::NonCopyable {
 public:
  static constexpr std::size_t kOutputLimit = 9U * 1024U * 1024U;
  static constexpr std::size_t kFileWriteBudget = 256U * 1024U;
  static constexpr std::size_t kFileCallBudget = 16;

  static bool outputFits(std::size_t pending, std::size_t incoming) noexcept;

  explicit ConnectionIo(Socket socket, std::size_t maxInputBytes = 0) noexcept;
  ConnectionIo(ConnectionIo&&) noexcept = default;
  ConnectionIo& operator=(ConnectionIo&&) noexcept = default;

  [[nodiscard]] int fd() const noexcept;

  [[nodiscard]] ReadResult readAvailable();
  [[nodiscard]] WriteResult writeAvailable();
  [[nodiscard]] ReadResult readOnce();

  [[nodiscard]] int socketError() const;

  [[nodiscard]] std::span<const std::byte> inputView() const noexcept;
  void consumeInputBytes(std::size_t count);

  void queueOutput(std::span<const std::byte> bytes);
  void queueFile(std::span<const std::byte> header, base::FileRegion file);

  void markPeerHalfClosed() noexcept;
  [[nodiscard]] bool peerHalfClosed() const noexcept;
  [[nodiscard]] bool hasPendingOutput() const noexcept;
  [[nodiscard]] bool acceptsInput() const noexcept;
  [[nodiscard]] std::size_t pendingBytes() const noexcept;
  [[nodiscard]] bool readyToClose() const noexcept;

 private:
  friend struct ConnectionIoTestAccess;

  Socket socket_;

  std::size_t maxInputBytes_{0};
  base::Buffer input_;

  base::Buffer output_{kOutputLimit};
  std::optional<base::FileRegion> file_;

  bool peerHalfClosed_{false};
};

}  // namespace hp::net
