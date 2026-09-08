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
                             ApplicationHandler handler, std::size_t max_input_bytes,
                             CloseCallback close_callback)
    : io_(std::move(socket), std::move(handler), max_input_bytes), identity_(identity),
      close_callback_(std::move(close_callback)),
      channel_(loop, io_.fd(), [this](std::uint32_t mask) { handle_event(mask); }) {
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
void TcpConnection::update_interest() {
    std::uint32_t events = io_.accepts_input() ? EPOLLIN | EPOLLRDHUP : 0U;
    if (io_.has_pending_output()) events |= EPOLLOUT;
    channel_.set_interest(events);
}
void TcpConnection::handle_event(std::uint32_t mask) noexcept {
    if (state_ != State::active) return;
    ++event_count_;
    last_mask_ = mask;
    try {
        last_result_ = io_.handle_event(mask);
        const auto& result = last_result_;
        if (result.socket_error_observed && result.socket_error)
            base::warn(error_message(fd(), "SO_ERROR", result.socket_error));
        if (result.socket_error_query_error)
            base::warn(error_message(fd(), "getsockopt(SO_ERROR)", result.socket_error_query_error));
        if (result.read_error) base::warn(error_message(fd(), "recv", result.read_error));
        if (result.write_error) base::warn(error_message(fd(), "send", result.write_error));
        if (result.write_would_block)
            base::info("S3 evidence: connection write reached EAGAIN with " +
                       std::to_string(io_.pending_bytes()) + " response bytes pending.");
        // An application callback may request local close. Never re-register it.
        if (state_ == State::closing) return;
        if (result.close_requested) { request_close(); return; }
        update_interest();
    } catch (const std::exception& error) {
        request_close();
        // Diagnostics must not defeat connection-level exception isolation.
        try { base::warn(std::string("connection event failed: ") + error.what()); }
        catch (...) {}
    } catch (...) {
        request_close();
        try { base::warn("connection event failed"); } catch (...) {}
    }
}
}
