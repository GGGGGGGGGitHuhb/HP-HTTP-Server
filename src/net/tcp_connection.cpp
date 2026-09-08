#include "net/tcp_connection.h"
#include "net/event_loop.h"
#include "base/logger.h"
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace hp::net {
namespace {
std::string error_message(int fd, const char* operation, int error) {
    return "connection fd " + std::to_string(fd) + " " + operation +
           " failed: " + std::generic_category().message(error) + " (" +
           std::to_string(error) + ")";
}
}
TcpConnection::TcpConnection(EventLoop& loop, Socket socket, Identity identity,
                             MessageCallback handler, std::size_t max_input_bytes,
                             CloseCallback close_callback)
    : io_(std::move(socket), max_input_bytes), message_callback_(std::move(handler)), identity_(identity),
      close_callback_(std::move(close_callback)),
      channel_(loop, io_.fd(), [this](std::uint32_t mask) { handle_event(mask); }) {
    if (!message_callback_) message_callback_ = [](TcpConnection& c, std::span<const std::byte> input, bool eof) {
        c.send(input);
        c.consume(input.size());
        if (eof) c.close_after_flush();
    };
    if (identity == 0 || !close_callback_) throw std::invalid_argument("invalid connection identity/callback");
}
TcpConnection::~TcpConnection() noexcept { stop(); }
void TcpConnection::start() {
    if (state_ == State::closing) throw std::logic_error("cannot restart closed connection");
    if (state_ == State::active) return;
    channel_.set_interest(EPOLLIN | EPOLLRDHUP);
    state_ = State::active;
}
void TcpConnection::stop() noexcept {
    state_ = State::closing;
    channel_.remove();
}
void TcpConnection::request_close() noexcept {
    if (state_ == State::closing) return;
    stop();
    close_callback_(fd(), identity_);
}
void TcpConnection::send(std::span<const std::byte> bytes) {
    if (state_ == State::closing) throw std::logic_error("send on closed connection");
    try {
        io_.queue_output(bytes); // Take independent storage before returning.
        if (!handling_event_ && state_ == State::active) {
            flush_output(); if (state_ == State::active) update_interest();
        }
    } catch (...) { request_close(); throw; }
}
void TcpConnection::consume(std::size_t count) { io_.consume(count); }
void TcpConnection::close_after_flush() {
    input_stopped_ = true;
    if (!io_.has_pending_output()) request_close();
    else if (!handling_event_ && state_ == State::active) {
        try { update_interest(); } catch (...) { request_close(); throw; }
    }
}
void TcpConnection::update_interest() {
    std::uint32_t events = !input_stopped_ && io_.accepts_input() ? EPOLLIN | EPOLLRDHUP : 0U;
    if (io_.has_pending_output()) events |= EPOLLOUT;
    channel_.set_interest(events);
}
void TcpConnection::read_messages() {
    while (state_ == State::active && !input_stopped_) {
        const auto read = io_.read_once();
        last_result_.bytes_read += read.bytes_read;
        last_result_.read_error = read.error_number;
        if (read.bytes_read || (read.peer_closed && !eof_notified_)) {
            if (read.peer_closed) eof_notified_ = true;
            ++message_count_;
            message_callback_(*this, io_.input_view(), read.peer_closed);
            // The callback may have consumed or invalidated the borrowed view.
        }
        if (read.peer_closed) input_stopped_ = true;
        if (read.error_number) last_result_.close_requested = true;
        if (!read.bytes_read) break;
    }
}
void TcpConnection::flush_output() {
    if (io_.has_pending_output()) {
        const auto written = io_.write_available();
        last_result_.bytes_written += written.bytes_written;
        last_result_.write_would_block = written.would_block;
        last_result_.write_error = written.error_number;
        if (written.error_number) last_result_.close_requested = true;
        if (written.would_block)
            base::info("S3 evidence: connection write reached EAGAIN with " +
                       std::to_string(io_.pending_bytes()) + " response bytes pending.");
    }
    if (input_stopped_ && !io_.has_pending_output()) last_result_.close_requested = true;
    if (!handling_event_ && last_result_.close_requested) request_close();
}
void TcpConnection::handle_event(std::uint32_t mask) noexcept {
    if (state_ != State::active) return;
    ++event_count_;
    last_mask_ = mask;
    last_result_ = {};
    handling_event_ = true;
    try {
        if (mask & EPOLLERR) {
            try { last_result_.socket_error = io_.socket_error(); last_result_.socket_error_observed = true; }
            catch (const std::system_error& e) { last_result_.socket_error_query_error = e.code().value(); }
            last_result_.close_requested = true;
        }
        if (mask & (EPOLLIN | EPOLLRDHUP)) read_messages();
        if (state_ == State::active) flush_output();
        if (mask & EPOLLHUP) last_result_.close_requested = true;
        const auto& result = last_result_;
        if (result.socket_error_observed && result.socket_error)
            base::warn(error_message(fd(), "SO_ERROR", result.socket_error));
        if (result.socket_error_query_error)
            base::warn(error_message(fd(), "getsockopt(SO_ERROR)", result.socket_error_query_error));
        if (result.read_error) base::warn(error_message(fd(), "recv", result.read_error));
        if (result.write_error) base::warn(error_message(fd(), "send", result.write_error));
        if (state_ == State::active) {
            if (result.close_requested) request_close();
            else update_interest();
        }
    } catch (const std::exception& error) {
        request_close();
        try { base::warn(std::string("connection event failed: ") + error.what()); } catch (...) {}
    } catch (...) {
        request_close();
        try { base::warn("connection event failed"); } catch (...) {}
    }
    handling_event_ = false;
}
}
