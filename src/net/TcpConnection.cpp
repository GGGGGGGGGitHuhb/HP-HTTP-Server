#include "net/TcpConnection.h"

#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include "base/Logger.h"
#include "net/EventLoop.h"

namespace hp::net {
namespace {
std::string errorMessage(int fd, const char* operation, int error) {
  return "connection fd " + std::to_string(fd) + " " + operation +
         " failed: " + std::generic_category().message(error) + " (" + std::to_string(error) + ")";
}
}  // namespace

TcpConnection::TcpConnection(EventLoop& ownerEventLoop,
                             Socket socket,
                             Identity identity,
                             std::size_t maxInputBytes)
    : io_(std::move(socket), maxInputBytes),
      identity_(identity),
      connectionChannel_(ownerEventLoop, io_.fd()) {
  if (identity == 0) throw std::invalid_argument("invalid connection identity");
  connectionChannel_.registerEventCallback(
      [this](std::uint32_t events) { handleConnectionEvent(events); });
}

TcpConnection::~TcpConnection() noexcept { deactivateConnection(); }

void TcpConnection::dispatchMessage(std::span<const std::byte> input, bool eof) {
  if (messageCallback_)
    messageCallback_(*this, input, eof);
  else
    handleEchoMessage(*this, input, eof);
}

void TcpConnection::handleEchoMessage(TcpConnection& connection,
                                      std::span<const std::byte> input,
                                      bool eof) {
  connection.sendBytes(input);
  connection.consumeInputBytes(input.size());
  if (eof) connection.closeAfterFlush();
}

void TcpConnection::activateConnection() {
  if (!closeCallback_) throw std::invalid_argument("missing connection close target");
  if (state_ == State::kClosing) throw std::logic_error("cannot restart closed connection");
  if (state_ == State::kActive) return;
  connectionChannel_.setInterest(EPOLLIN | EPOLLRDHUP);
  state_ = State::kActive;
}

void TcpConnection::deactivateConnection() noexcept {
  state_ = State::kClosing;
  connectionChannel_.removeChannel();
  if (timeoutActivityCallback_) timeoutActivityCallback_(*this, false);
}

void TcpConnection::requestClose() noexcept {
  if (state_ == State::kClosing) return;
  deactivateConnection();
  closeCallback_(fd(), identity_);
}

void TcpConnection::sendBytes(std::span<const std::byte> bytes) {
  if (state_ == State::kClosing || draining_)
    throw std::logic_error("send on closed or draining connection");
  try {
    io_.queueOutput(bytes);  // 在返回前取得独立存储。
    // handlingEvent 在 TcpConnection::handleConnectionEvent() 中被置 true
    if (!handlingEvent_ && state_ == State::kActive) {
      if (!writeCompleteCallback_) flushOutput();
      if (state_ == State::kActive) updateInterest();
    }
  } catch (...) {
    requestClose();
    throw;
  }
}

void TcpConnection::sendFile(std::span<const std::byte> header, base::FileRegion file) {
  if (state_ == State::kClosing || draining_)
    throw std::logic_error("send on closed or draining connection");
  try {
    io_.queueFile(header, std::move(file));
    if (!handlingEvent_ && state_ == State::kActive) {
      if (!writeCompleteCallback_) flushOutput();
      if (state_ == State::kActive) updateInterest();
    }
  } catch (...) {
    requestClose();
    throw;
  }
}

// 从连接的输入缓冲区中移除这次已经接受的字节
void TcpConnection::consumeInputBytes(std::size_t count) { io_.consumeInputBytes(count); }

void TcpConnection::beginConnectionDrain() {
  if (state_ == State::kClosing || draining_) return;
  draining_ = true;
  inputStopped_ = true;
  readPaused_ = true;
  idleWaiting_ = false;
  // 保留终结通知；上层看到draining后只完成当前响应，不推进下一请求。
  if (timeoutActivityCallback_) timeoutActivityCallback_(*this, false);
  if (!io_.hasPendingOutput())
    requestClose();
  else
    updateInterest();
}

void TcpConnection::closeAfterFlush() {
  inputStopped_ = true;  // 停止后续输入
  // 输出排空才关闭
  if (!io_.hasPendingOutput())
    requestClose();
  else if (!handlingEvent_ && state_ == State::kActive) {
    try {
      updateInterest();
    } catch (...) {
      requestClose();
      throw;
    }
  }
}

void TcpConnection::pauseReading() {
  // 暂停读取
  readPaused_ = true;
  if (!handlingEvent_ && state_ == State::kActive) updateInterest();
}

void TcpConnection::resumeReading() {
  // 解除暂停
  readPaused_ = false;
  // 在 handleConnectionEvent() 执行时，不会从此处更新 interest
  if (!handlingEvent_ && state_ == State::kActive) updateInterest();
}

void TcpConnection::setIdleWait(bool waiting) {
  if (idleWaiting_ == waiting) return;
  idleWaiting_ = waiting;
  if (timeoutActivityCallback_) timeoutActivityCallback_(*this, false);
}

void TcpConnection::updateInterest() {
  std::uint32_t events =
      !inputStopped_ && !readPaused_ && io_.acceptsInput() ? EPOLLIN | EPOLLRDHUP : 0U;
  if (io_.hasPendingOutput()) events |= EPOLLOUT;
  connectionChannel_.setInterest(events);
}

void TcpConnection::readMessages() {
  while (state_ == State::kActive && !inputStopped_ && !readPaused_) {
    const auto read = io_.readOnce();
    if (read.bytesRead) {
      idleWaiting_ = false;
      if (timeoutActivityCallback_) timeoutActivityCallback_(*this, true);
    }
    lastResult_.bytesRead += read.bytesRead;
    lastResult_.readError = read.errorNumber;
    // 收到数据或首次观察到对端发送结束(EOF)
    if (read.bytesRead || (read.peerClosed && !eofNotified_)) {
      if (read.peerClosed) eofNotified_ = true;
      ++messageCount_;
      dispatchMessage(io_.inputView(), read.peerClosed);
      // 回调可能已经消费输入或使借用的视图失效。
    }
    if (read.peerClosed && !writeCompleteCallback_) inputStopped_ = true;
    if (read.errorNumber) lastResult_.closeRequested = true;
    if (!read.bytesRead) break;
  }
}

void TcpConnection::flushOutput() {
  while (state_ == State::kActive && io_.hasPendingOutput()) {
    const auto written = io_.writeAvailable();
    if (written.bytesWritten && timeoutActivityCallback_) timeoutActivityCallback_(*this, true);
    lastResult_.bytesWritten += written.bytesWritten;
    lastResult_.writeWouldBlock = written.wouldBlock;
    lastResult_.writeError = written.errorNumber;
    // 写入报错
    if (written.errorNumber) {
      lastResult_.closeRequested = true;
      break;
    }
    if (written.wouldBlock) {
      base::info("S3 evidence: connection write reached EAGAIN with " +
                 std::to_string(io_.pendingBytes()) + " response bytes pending.");
      break;
    }
    // 必须响应排空
    if (!io_.hasPendingOutput() && written.bytesWritten && writeCompleteCallback_ &&
        (!inputStopped_ || draining_)) {
      // 回调中的 Send 只排队：由外层循环推进下一个响应。
      writeCompleteCallback_(*this);
    } else
      break;
    if (written.fileTransfer)
      break;  // 不得通过重入的 HTTP
              // 回调再次消耗文件发送预算。
  }
  if (inputStopped_ && !io_.hasPendingOutput()) lastResult_.closeRequested = true;
  if (!handlingEvent_ && lastResult_.closeRequested) requestClose();
}

void TcpConnection::handleConnectionEvent(std::uint32_t mask) noexcept {
  if (state_ != State::kActive) return;
  ++eventCount_;
  lastMask_ = mask;
  lastResult_ = {};
  handlingEvent_ = true;
  try {
    if (mask & EPOLLERR) {
      try {
        lastResult_.socketError = io_.socketError();
        lastResult_.socketErrorObserved = true;
      } catch (const std::system_error& e) {
        lastResult_.socketErrorQueryError = e.code().value();
      }
      lastResult_.closeRequested = true;
    }
    if (mask & (EPOLLIN | EPOLLRDHUP)) readMessages();
    if (state_ == State::kActive) flushOutput();
    if (mask & EPOLLHUP) lastResult_.closeRequested = true;
    const auto& result = lastResult_;
    if (result.socketErrorObserved && result.socketError)
      base::warn(errorMessage(fd(), "SO_ERROR", result.socketError));
    if (result.socketErrorQueryError)
      base::warn(errorMessage(fd(), "getsockopt(SO_ERROR)", result.socketErrorQueryError));
    if (result.readError) base::warn(errorMessage(fd(), "recv", result.readError));
    if (result.writeError) base::warn(errorMessage(fd(), "send", result.writeError));
    if (state_ == State::kActive) {
      if (result.closeRequested)
        requestClose();
      else
        // 统一更新事件掩码
        updateInterest();
    }
  } catch (const std::exception& error) {
    requestClose();
    try {
      base::warn(std::string("connection event failed: ") + error.what());
    } catch (...) {
    }
  } catch (...) {
    requestClose();
    try {
      base::warn("connection event failed");
    } catch (...) {
    }
  }
  handlingEvent_ = false;
}
}  // namespace hp::net
