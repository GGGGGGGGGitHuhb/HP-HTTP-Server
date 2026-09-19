#include "net/tcp_connection.h"

#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include "base/logger.h"
#include "net/event_loop.h"

namespace hp::net {
namespace {
std::string ErrorMessage(int fd, const char* operation, int error) {
  return "connection fd " + std::to_string(fd) + " " + operation +
         " failed: " + std::generic_category().message(error) + " (" +
         std::to_string(error) + ")";
}
}  // namespace

TcpConnection::TcpConnection(EventLoop& loop,
                             Socket socket,
                             Identity identity,
                             std::size_t max_input_bytes)
    : io_(std::move(socket), max_input_bytes),
      message_callback_(&TcpConnection::HandleEchoMessage),
      identity_(identity),
      connection_channel_(loop, io_.fd()) {
  if (identity == 0) throw std::invalid_argument("invalid connection identity");
  connection_channel_.set_HandleConnectionEvent_callback(
      std::bind_front(&TcpConnection::HandleConnectionEvent, this));
}

void TcpConnection::set_HandleMessage_callback(
    MessageCallback message_callback) {
  message_callback_ = message_callback
                          ? std::move(message_callback)
                          : MessageCallback{&TcpConnection::HandleEchoMessage};
}

void TcpConnection::set_OnConnectionClosed_callback(
    CloseCallback close_callback) {
  close_callback_ = std::move(close_callback);
}

void TcpConnection::set_UpdateTimeout_callback(
    std::function<void(TcpConnection&, bool)> timeout_activity_callback) {
  timeout_activity_callback_ = std::move(timeout_activity_callback);
}

TcpConnection::~TcpConnection() noexcept { Stop(); }

void TcpConnection::HandleEchoMessage(TcpConnection& connection,
                                      std::span<const std::byte> input,
                                      bool eof) {
  connection.Send(input);
  connection.Consume(input.size());
  if (eof) connection.CloseAfterFlush();
}

void TcpConnection::Start() {
  if (!close_callback_)
    throw std::invalid_argument("missing connection close target");
  if (state_ == State::kClosing)
    throw std::logic_error("cannot restart closed connection");
  if (state_ == State::kActive) return;
  connection_channel_.set_interest(EPOLLIN | EPOLLRDHUP);
  state_ = State::kActive;
}

void TcpConnection::Stop() noexcept {
  state_ = State::kClosing;
  connection_channel_.Remove();
  if (timeout_activity_callback_) timeout_activity_callback_(*this, false);
}

void TcpConnection::RequestClose() noexcept {
  if (state_ == State::kClosing) return;
  Stop();
  close_callback_(fd(), identity_);
}

void TcpConnection::Send(std::span<const std::byte> bytes) {
  if (state_ == State::kClosing || draining_)
    throw std::logic_error("send on closed or draining connection");
  try {
    io_.QueueOutput(bytes);  // Take independent storage before returning.
    if (!handling_event_ && state_ == State::kActive) {
      if (!write_complete_callback_) FlushOutput();
      if (state_ == State::kActive) UpdateInterest();
    }
  } catch (...) {
    RequestClose();
    throw;
  }
}

void TcpConnection::SendFile(std::span<const std::byte> header,
                             base::FileRegion file) {
  if (state_ == State::kClosing || draining_)
    throw std::logic_error("send on closed or draining connection");
  try {
    io_.QueueFile(header, std::move(file));
    if (!handling_event_ && state_ == State::kActive) {
      if (!write_complete_callback_) FlushOutput();
      if (state_ == State::kActive) UpdateInterest();
    }
  } catch (...) {
    RequestClose();
    throw;
  }
}

void TcpConnection::Consume(std::size_t count) { io_.Consume(count); }

void TcpConnection::BeginDrain() {
  if (state_ == State::kClosing || draining_) return;
  draining_ = true;
  input_stopped_ = true;
  read_paused_ = true;
  idle_waiting_ = false;
  // Suppress Session::HandleWriteComplete before any future write can finish a
  // response.
  write_complete_callback_ = {};
  if (timeout_activity_callback_) timeout_activity_callback_(*this, false);
  if (!io_.has_pending_output())
    RequestClose();
  else
    UpdateInterest();
}

void TcpConnection::CloseAfterFlush() {
  input_stopped_ = true;
  if (!io_.has_pending_output())
    RequestClose();
  else if (!handling_event_ && state_ == State::kActive) {
    try {
      UpdateInterest();
    } catch (...) {
      RequestClose();
      throw;
    }
  }
}

void TcpConnection::set_HandleWriteComplete_callback(
    WriteCompleteCallback write_complete_callback) {
  write_complete_callback_ = std::move(write_complete_callback);
}

void TcpConnection::PauseReading() {
  read_paused_ = true;
  if (!handling_event_ && state_ == State::kActive) UpdateInterest();
}

void TcpConnection::ResumeReading() {
  read_paused_ = false;
  if (!handling_event_ && state_ == State::kActive) UpdateInterest();
}

void TcpConnection::set_idle_wait(bool waiting) {
  if (idle_waiting_ == waiting) return;
  idle_waiting_ = waiting;
  if (timeout_activity_callback_) timeout_activity_callback_(*this, false);
}

void TcpConnection::UpdateInterest() {
  std::uint32_t events = !input_stopped_ && !read_paused_ && io_.accepts_input()
                             ? EPOLLIN | EPOLLRDHUP
                             : 0U;
  if (io_.has_pending_output()) events |= EPOLLOUT;
  connection_channel_.set_interest(events);
}

void TcpConnection::ReadMessages() {
  while (state_ == State::kActive && !input_stopped_ && !read_paused_) {
    const auto read = io_.ReadOnce();
    if (read.bytes_read) {
      idle_waiting_ = false;
      if (timeout_activity_callback_) timeout_activity_callback_(*this, true);
    }
    last_result_.bytes_read += read.bytes_read;
    last_result_.read_error = read.error_number;
    if (read.bytes_read || (read.peer_closed && !eof_notified_)) {
      if (read.peer_closed) eof_notified_ = true;
      ++message_count_;
      message_callback_(*this, io_.input_view(), read.peer_closed);
      // The callback may have consumed or invalidated the borrowed view.
    }
    if (read.peer_closed && !write_complete_callback_) input_stopped_ = true;
    if (read.error_number) last_result_.close_requested = true;
    if (!read.bytes_read) break;
  }
}

void TcpConnection::FlushOutput() {
  while (state_ == State::kActive && io_.has_pending_output()) {
    const auto written = io_.WriteAvailable();
    if (written.bytes_written && timeout_activity_callback_)
      timeout_activity_callback_(*this, true);
    last_result_.bytes_written += written.bytes_written;
    last_result_.write_would_block = written.would_block;
    last_result_.write_error = written.error_number;
    if (written.error_number) {
      last_result_.close_requested = true;
      break;
    }
    if (written.would_block) {
      base::info("S3 evidence: connection write reached EAGAIN with " +
                 std::to_string(io_.pending_bytes()) +
                 " response bytes pending.");
      break;
    }
    if (!io_.has_pending_output() && written.bytes_written &&
        write_complete_callback_ && !input_stopped_) {
      // Callback Send only queues: the outer loop drives the next response.
      write_complete_callback_(*this);
    } else
      break;
    if (written.file_transfer)
      break;  // Do not spend another file budget through a reentrant HTTP
              // callback.
  }
  if (input_stopped_ && !io_.has_pending_output())
    last_result_.close_requested = true;
  if (!handling_event_ && last_result_.close_requested) RequestClose();
}

void TcpConnection::HandleConnectionEvent(std::uint32_t mask) noexcept {
  if (state_ != State::kActive) return;
  ++event_count_;
  last_mask_ = mask;
  last_result_ = {};
  handling_event_ = true;
  try {
    if (mask & EPOLLERR) {
      try {
        last_result_.socket_error = io_.socket_error();
        last_result_.socket_error_observed = true;
      } catch (const std::system_error& e) {
        last_result_.socket_error_query_error = e.code().value();
      }
      last_result_.close_requested = true;
    }
    if (mask & (EPOLLIN | EPOLLRDHUP)) ReadMessages();
    if (state_ == State::kActive) FlushOutput();
    if (mask & EPOLLHUP) last_result_.close_requested = true;
    const auto& result = last_result_;
    if (result.socket_error_observed && result.socket_error)
      base::warn(ErrorMessage(fd(), "SO_ERROR", result.socket_error));
    if (result.socket_error_query_error)
      base::warn(ErrorMessage(fd(),
                              "getsockopt(SO_ERROR)",
                              result.socket_error_query_error));
    if (result.read_error)
      base::warn(ErrorMessage(fd(), "recv", result.read_error));
    if (result.write_error)
      base::warn(ErrorMessage(fd(), "send", result.write_error));
    if (state_ == State::kActive) {
      if (result.close_requested)
        RequestClose();
      else
        UpdateInterest();
    }
  } catch (const std::exception& error) {
    RequestClose();
    try {
      base::warn(std::string("connection event failed: ") + error.what());
    } catch (...) {
    }
  } catch (...) {
    RequestClose();
    try {
      base::warn("connection event failed");
    } catch (...) {
    }
  }
  handling_event_ = false;
}
}  // namespace hp::net
