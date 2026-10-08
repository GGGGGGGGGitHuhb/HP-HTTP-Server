#include "RequestBoundaryObserver.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <limits>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace hp::net {
namespace {
bool writeAll(int fd, const unsigned char* bytes, std::size_t count) noexcept {
  while (count) {
    const auto written = ::write(fd, bytes, count);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return false;
    bytes += written;
    count -= static_cast<std::size_t>(written);
  }
  return true;
}

void encodeLittle(unsigned char* bytes, std::uint64_t value, unsigned width) noexcept {
  for (unsigned index = 0; index < width; ++index) bytes[index] = value >> (index * 8);
}
}  // namespace

BoundaryOutputDirectory::~BoundaryOutputDirectory() noexcept { closeDirectory(); }

bool BoundaryOutputDirectory::openDirectory(const char* path) noexcept {
  if (fd_ >= 0) return false;
  fd_ = ::open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  return fd_ >= 0;
}

bool BoundaryOutputDirectory::closeDirectory() noexcept {
  if (fd_ < 0) return true;
  const int descriptor = fd_;
  fd_ = -1;
  return ::close(descriptor) == 0;
}

RequestBoundaryObserver::RequestBoundaryObserver(std::uint32_t workerIndex,
                                               std::size_t recordCapacity)
    : workerIndex_(workerIndex), recordCapacity_(recordCapacity),
      records_(std::make_unique<Record[]>(recordCapacity)) {
  if (!recordCapacity || recordCapacity > kRecordCapacity)
    throw std::invalid_argument("boundary capacity");
}

std::uint64_t RequestBoundaryObserver::monotonicNs() noexcept {
  timespec value{};
  if (::clock_gettime(CLOCK_MONOTONIC, &value) || value.tv_sec < 0) return 0;
  return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL + value.tv_nsec;
}

std::uint64_t RequestBoundaryObserver::processStarttime() noexcept {
  try {
    std::ifstream input("/proc/self/stat");
    std::string line;
    if (!std::getline(input, line)) return 0;
    const auto ending = line.rfind(')');
    if (ending == std::string::npos) return 0;
    std::istringstream fields(line.substr(ending + 2));
    std::string field;
    for (unsigned index = 3; index <= 22; ++index) {
      if (!(fields >> field)) return 0;
    }
    std::size_t consumed = 0;
    const auto value = std::stoull(field, &consumed);
    return consumed == field.size() ? value : 0;
  } catch (...) { return 0; }
}

bool RequestBoundaryObserver::readClockIdentity(ClockIdentity& identity) noexcept {
  try {
    std::ifstream boot("/proc/sys/kernel/random/boot_id");
    if (!std::getline(boot, identity.bootId) || identity.bootId.size() != 36) return false;
    char link[128]{};
    const auto count = ::readlink("/proc/thread-self/ns/time", link, sizeof(link) - 1);
    if (count <= 0 || count >= static_cast<ssize_t>(sizeof(link) - 1)) return false;
    identity.timeNamespace.assign(link, count);
    timespec resolution{};
    if (::clock_getres(CLOCK_MONOTONIC, &resolution)) return false;
    identity.resolutionNs = static_cast<std::uint64_t>(resolution.tv_sec) * 1000000000ULL + resolution.tv_nsec;
    identity.observedNs = monotonicNs();
    identity.processStarttime = processStarttime();
    return identity.resolutionNs && identity.observedNs && identity.processStarttime;
  } catch (...) { return false; }
}

void RequestBoundaryObserver::markInvalid(const char* reason) noexcept {
  if (!invalidReason_) invalidReason_ = reason;
}

void RequestBoundaryObserver::initializeWriter(std::uint64_t processStarttime) noexcept {
  pid_ = ::getpid();
  tid_ = ::syscall(SYS_gettid);
  processStarttime_ = processStarttime;
  if (!processStarttime_ || !tid_ || !readClockIdentity(clockStart_) || clockStart_.processStarttime != processStarttime_) markInvalid("writer_identity");
}

RequestBoundaryObserver::Connection* RequestBoundaryObserver::registerConnection(int fd) noexcept {
  if (writerStopped_ || connectionCount_ == kConnectionCapacity) {
    markInvalid("connection_capacity_or_stopped");
    return nullptr;
  }
  sockaddr_in local{}, peer{};
  socklen_t localSize = sizeof(local), peerSize = sizeof(peer);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &localSize) ||
      ::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &peerSize) ||
      localSize != sizeof(local) || peerSize != sizeof(peer) ||
      local.sin_family != AF_INET || peer.sin_family != AF_INET) {
    markInvalid("connection_tuple");
    return nullptr;
  }
  for (std::size_t index = 0; index < connectionCount_; ++index) {
    const auto& previous = connections_[index];
    if (!previous.closedNs && (previous.fd == fd ||
        (previous.clientAddress == ntohl(peer.sin_addr.s_addr) && previous.clientPort == ntohs(peer.sin_port) &&
         previous.serverAddress == ntohl(local.sin_addr.s_addr) && previous.serverPort == ntohs(local.sin_port)))) {
      markInvalid("duplicate_live_connection");
      return nullptr;
    }
  }
  auto& connection = connections_[connectionCount_++];
  connection.connectionId = static_cast<std::uint32_t>(connectionCount_);
  connection.fd = fd;
  connection.clientAddress = ntohl(peer.sin_addr.s_addr);
  connection.clientPort = ntohs(peer.sin_port);
  connection.serverAddress = ntohl(local.sin_addr.s_addr);
  connection.serverPort = ntohs(local.sin_port);
  connection.openedNs = monotonicNs();
  if (!connection.openedNs) markInvalid("connection_clock");
  return &connection;
}

void RequestBoundaryObserver::observeRead(Connection* connection, std::uint64_t readNs) noexcept {
  if (!connection) return;
  if (!readNs || writerStopped_ || connection->closedNs) {
    markInvalid("read_state_or_clock");
    return;
  }
  if (connection->pendingRecord != kRecordCapacity) return;  // 分片续读不增加序号。
  if (recordCount_ == recordCapacity_) {
    overflow_ = true;
    markInvalid("record_capacity");
    return;
  }
  if (connection->sequence == std::numeric_limits<std::uint64_t>::max()) {
    markInvalid("sequence_overflow");
    return;
  }
  connection->pendingRecord = recordCount_++;
  connection->parsed = false;
  connection->partialEof = false;
  records_[connection->pendingRecord] = {connection->connectionId, 1,
                                         ++connection->sequence, readNs, 0};
}

void RequestBoundaryObserver::observeParsed(Connection* connection) noexcept {
  if (!connection) return;
  if (connection->pendingRecord == kRecordCapacity || connection->parsed)
    markInvalid("parse_without_read_or_pipeline");
  connection->parsed = true;
}

void RequestBoundaryObserver::observePartialEof(Connection* connection) noexcept {
  if (!connection || connection->pendingRecord == kRecordCapacity || connection->parsed) {
    markInvalid("partial_eof_state");
    return;
  }
  connection->partialEof = true;
}

void RequestBoundaryObserver::observeDrained(Connection* connection,
                                           std::uint64_t drainedNs) noexcept {
  if (!connection) return;
  if (writerStopped_ || connection->pendingRecord == kRecordCapacity) {
    markInvalid("drain_without_request");
    return;
  }
  if (!connection->parsed) return;  // EOF末尾部分请求不伪造完整D，离线须验证client终态。
  auto& request = records_[connection->pendingRecord];
  if (!drainedNs || drainedNs < request.readNs) {
    markInvalid("drain_clock");
    return;
  }
  request.drainedNs = drainedNs;
  request.flags = 3;
  ++completedCount_;
  connection->pendingRecord = kRecordCapacity;
  connection->parsed = false;
}

void RequestBoundaryObserver::observeClosed(Connection* connection,
                                          std::uint64_t closedNs) noexcept {
  if (!connection) return;
  if (!closedNs || connection->closedNs) markInvalid("close_clock_or_duplicate");
  connection->closedNs = closedNs;
}

void RequestBoundaryObserver::stopWriter() noexcept { writerStopped_ = true; }

bool RequestBoundaryObserver::writeHeader(int fd, const char* magic) const noexcept {
  unsigned char bytes[96]{};
  for (unsigned index = 0; index < 8; ++index) bytes[index] = magic[index];
  encodeLittle(bytes + 8, 1, 4);
  encodeLittle(bytes + 12, workerIndex_, 4);
  const std::uint64_t values[] = {pid_, tid_, processStarttime_, recordCount_, completedCount_,
      recordCount_ - completedCount_, overflow_ ? 1ULL : 0ULL, writerStopped_ ? 1ULL : 0ULL, 1, 0};
  for (unsigned index = 0; index < 10; ++index) encodeLittle(bytes + 16 + index * 8, values[index], 8);
  return writeAll(fd, bytes, sizeof(bytes));
}

bool RequestBoundaryObserver::exportRecords(int directoryFd) const noexcept {
  if (!writerStopped_) return false;
  char name[64];
  std::snprintf(name, sizeof(name), "boundary-worker-%u.bin", workerIndex_);
  const int fd = ::openat(directoryFd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  bool succeeded = writeHeader(fd, "HPBOUND1");
  // 停止后分块编码；测量期间不执行文件写入。
  unsigned char block[8192]{};
  for (std::size_t first = 0; first < recordCount_ && succeeded; first += 256) {
    const auto count = std::min<std::size_t>(256, recordCount_ - first);
    for (std::size_t offset = 0; offset < count; ++offset) {
      const auto& request = records_[first + offset];
      auto* bytes = block + offset * 32;
      encodeLittle(bytes, request.connectionId, 4);
      encodeLittle(bytes + 4, request.flags, 4);
      encodeLittle(bytes + 8, request.sequence, 8);
      encodeLittle(bytes + 16, request.readNs, 8);
      encodeLittle(bytes + 24, request.drainedNs, 8);
    }
    succeeded = writeAll(fd, block, count * 32);
  }
  if (succeeded) succeeded = writeHeader(fd, "HPBTAIL1");
  if (::close(fd)) succeeded = false;
  return exportConnections(directoryFd) && succeeded && valid();
}

bool RequestBoundaryObserver::exportConnections(int directoryFd) const noexcept {
  char name[64];
  std::snprintf(name, sizeof(name), "boundary-worker-%u.json", workerIndex_);
  const int fd = ::openat(directoryFd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  FILE* output = ::fdopen(fd, "w");
  if (!output) { ::close(fd); return false; }
  std::fprintf(output, "{\"schema\":\"request-boundaries-worker-v1\",\"worker\":%u,\"pid\":%llu,\"tid\":%llu,\"process_starttime\":%llu,\"invalid_reason\":\"%s\",",
      workerIndex_, static_cast<unsigned long long>(pid_), static_cast<unsigned long long>(tid_),
      static_cast<unsigned long long>(processStarttime_), invalidReason_ ? invalidReason_ : "");
  ClockIdentity clockStop;
  const bool clockValid = readClockIdentity(clockStop) && clockStop.processStarttime == processStarttime_ &&
      clockStop.bootId == clockStart_.bootId && clockStop.timeNamespace == clockStart_.timeNamespace &&
      clockStop.observedNs >= clockStart_.observedNs;
  const ClockIdentity* clocks[] = {&clockStart_, &clockStop};
  for (unsigned index = 0; index < 2; ++index) {
    const auto& identity = *clocks[index];
    std::fprintf(output, "\"clock_%s\":{\"clock\":\"CLOCK_MONOTONIC\",\"boot_id\":\"%s\",\"time_namespace\":\"%s\",\"resolution_ns\":%llu,\"observed_ns\":%llu,\"process_starttime\":%llu},",
        index ? "stop" : "start", identity.bootId.c_str(), identity.timeNamespace.c_str(),
        static_cast<unsigned long long>(identity.resolutionNs), static_cast<unsigned long long>(identity.observedNs),
        static_cast<unsigned long long>(identity.processStarttime));
  }
  std::fprintf(output, "\"record_count\":%zu,\"completed\":%zu,\"incomplete\":%zu,\"overflow\":%s,\"writer_stopped\":%s,\"connections\":[",
      recordCount_, completedCount_, recordCount_ - completedCount_, overflow_ ? "true" : "false", writerStopped_ ? "true" : "false");
  for (std::size_t index = 0; index < connectionCount_; ++index) {
    const auto& connection = connections_[index];
    std::fprintf(output, "%s{\"connection_id\":%u,\"fd\":%d,\"client_address_u32\":%u,\"client_port\":%u,\"server_address_u32\":%u,\"server_port\":%u,\"opened_ns\":%llu,\"closed_ns\":%llu,\"last_sequence\":%llu,\"tail_request_parsed\":%s,\"partial_eof\":%s}",
        index ? "," : "", connection.connectionId, connection.fd, connection.clientAddress,
        connection.clientPort, connection.serverAddress, connection.serverPort,
        static_cast<unsigned long long>(connection.openedNs), static_cast<unsigned long long>(connection.closedNs),
        static_cast<unsigned long long>(connection.sequence), connection.parsed ? "true" : "false", connection.partialEof ? "true" : "false");
  }
  std::fputs("]}\n", output);
  bool succeeded = clockValid && !std::ferror(output);
  if (::fclose(output)) succeeded = false;
  return succeeded;
}
}  // namespace hp::net
