#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace hp::net {
class BoundaryOutputDirectory final {
 public:
  BoundaryOutputDirectory() = default;
  ~BoundaryOutputDirectory() noexcept;
  BoundaryOutputDirectory(const BoundaryOutputDirectory&) = delete;
  BoundaryOutputDirectory& operator=(const BoundaryOutputDirectory&) = delete;

  bool openDirectory(const char* path) noexcept;
  bool closeDirectory() noexcept;
  int fd() const noexcept { return fd_; }

 private:
  int fd_ = -1;
};

class RequestBoundaryObserver final {
 public:
  static constexpr std::size_t kRecordCapacity = 524288;
  static constexpr std::size_t kConnectionCapacity = 256;

  struct ClockIdentity {
    std::string bootId;
    std::string timeNamespace;
    std::uint64_t resolutionNs = 0;
    std::uint64_t observedNs = 0;
    std::uint64_t processStarttime = 0;
  };

  struct Record {
    std::uint32_t connectionId = 0;
    std::uint32_t flags = 0;
    std::uint64_t sequence = 0;
    std::uint64_t readNs = 0;
    std::uint64_t drainedNs = 0;
  };

  struct Connection {
    std::uint32_t connectionId = 0;
    int fd = -1;
    std::uint32_t clientAddress = 0;
    std::uint16_t clientPort = 0;
    std::uint32_t serverAddress = 0;
    std::uint16_t serverPort = 0;
    std::uint64_t openedNs = 0;
    std::uint64_t closedNs = 0;
    std::uint64_t sequence = 0;
    std::size_t pendingRecord = kRecordCapacity;
    bool parsed = false;
    bool partialEof = false;
  };

  explicit RequestBoundaryObserver(std::uint32_t workerIndex,
                                   std::size_t recordCapacity = kRecordCapacity);

  void initializeWriter(std::uint64_t processStarttime) noexcept;
  Connection* registerConnection(int fd) noexcept;
  void observeRead(Connection* connection, std::uint64_t readNs) noexcept;
  void observeParsed(Connection* connection) noexcept;
  void observePartialEof(Connection* connection) noexcept;
  void observeDrained(Connection* connection, std::uint64_t drainedNs) noexcept;
  void observeClosed(Connection* connection, std::uint64_t closedNs) noexcept;
  void stopWriter() noexcept;
  bool exportRecords(int directoryFd) const noexcept;
  void markInvalid(const char* reason) noexcept;

  static std::uint64_t monotonicNs() noexcept;
  static std::uint64_t processStarttime() noexcept;
  bool valid() const noexcept { return invalidReason_ == nullptr && !overflow_; }
  const char* invalidReason() const noexcept { return invalidReason_; }
  std::size_t recordCount() const noexcept { return recordCount_; }
  const Record& record(std::size_t index) const noexcept { return records_[index]; }

 private:
  static bool readClockIdentity(ClockIdentity& identity) noexcept;
  bool writeHeader(int fd, const char* magic) const noexcept;
  bool exportConnections(int directoryFd) const noexcept;

  const std::uint32_t workerIndex_;
  const std::size_t recordCapacity_;
  std::unique_ptr<Record[]> records_;
  std::array<Connection, kConnectionCapacity> connections_{};
  std::size_t connectionCount_ = 0;
  std::size_t recordCount_ = 0;
  std::size_t completedCount_ = 0;
  std::uint64_t pid_ = 0;
  std::uint64_t tid_ = 0;
  std::uint64_t processStarttime_ = 0;
  ClockIdentity clockStart_;
  const char* invalidReason_ = nullptr;
  bool overflow_ = false;
  bool writerStopped_ = false;
};
}  // namespace hp::net
