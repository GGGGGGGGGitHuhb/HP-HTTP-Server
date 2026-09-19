#include "http_connection_handler.h"

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "base/logger.h"
#include "http/http_response.h"

namespace hp::app {
namespace {
using SessionObserver = void (*)(bool, const void*) noexcept;
std::atomic<SessionObserver> session_observer{nullptr};

}  // namespace

Session::Session(HttpCallbackStats* callback_stats) : stats(callback_stats) {
  if (auto observer = session_observer.load()) observer(true, this);
}

Session::~Session() {
  if (auto observer = session_observer.load()) observer(false, this);
}

void Session::set_PrepareResponse_callback(ResponseProvider response_provider) {
  provider = std::move(response_provider);
}

void Session::HandleMessage(net::TcpConnection& connection) {
  if (phase != Phase::kReading) return;
  const auto input = connection.input_view();
  const bool eof = connection.peer_closed();
  if (stats) {
    ++stats->parses;
    if (eof) ++stats->eof_notifications;
  }
  const auto parsed =
      parser.Feed({reinterpret_cast<const char*>(input.data()), input.size()});
  if (stats) {
    stats->submitted_bytes += input.size();
    stats->accepted_bytes += parsed.accepted_bytes;
  }
  connection.Consume(
      parsed.accepted_bytes);  // Never use the borrowed input again.
  if (stats) stats->consumed_bytes += parsed.accepted_bytes;
  if (parsed.status == http::ParseStatus::kNeedMore && !eof) {
    if (stats) ++stats->need_more;
    connection.set_idle_wait(completed && parsed.request_bytes == 0 &&
                             connection.pending_bytes() == 0);
    connection.ResumeReading();
    return;
  }
  if (parsed.status == http::ParseStatus::kNeedMore && completed &&
      parsed.request_bytes == 0) {
    phase = Phase::kClosing;
    connection.CloseAfterFlush();
    return;
  }
  close = parsed.status != http::ParseStatus::kComplete ||
          parsed.request.close_requested;
  const auto policy = close ? http::ConnectionPolicy::kClose
                            : http::ConnectionPolicy::kKeepAlive;
  connection.set_idle_wait(false);
  phase = Phase::kWriting;
  connection.PauseReading();
  std::vector<std::byte> response;
  std::optional<base::FileRegion> file;
  try {
    switch (parsed.status) {
      case http::ParseStatus::kNeedMore:
      case http::ParseStatus::kBadRequest:
        response = http::MakeErrorResponse(http::Status::kBadRequest, policy);
        break;
      case http::ParseStatus::kMethodNotAllowed:
        response =
            http::MakeErrorResponse(http::Status::kMethodNotAllowed, policy);
        break;
      case http::ParseStatus::kComplete: {
        auto result = provider(parsed.request, policy);
        if (close && result.effective_policy != http::ConnectionPolicy::kClose)
          throw std::logic_error("provider relaxed terminal connection policy");
        close = result.effective_policy == http::ConnectionPolicy::kClose;
        response = std::move(result.bytes);
        file = std::move(result.file);
        break;
      }
    }
  } catch (...) {
    close = true;
    file.reset();
    response = http::MakeErrorResponse(http::Status::kInternalServerError,
                                       http::ConnectionPolicy::kClose);
  }
  if (file)
    connection.SendFile(response, std::move(*file));
  else
    connection.Send(response);
  completed = true;
  if (stats) ++stats->responses;
  base::Info(
      "S3 evidence: HTTP message callback produced one response via "
      "incremental parser.");
}

void Session::HandleWriteComplete(net::TcpConnection& connection) {
  if (phase != Phase::kWriting) return;
  if (close) {
    phase = Phase::kClosing;
    connection.CloseAfterFlush();
    return;
  }
  parser.Reset();
  phase = Phase::kReading;
  // Process transport-owned suffix even without a new readable event.
  HandleMessage(connection);
}

void HttpMessageHandler::set_PrepareResponse_callback(
    ResponseProvider response_provider) {
  provider = std::move(response_provider);
}

void HttpMessageHandler::HandleMessage(net::TcpConnection& connection,
                                       std::span<const std::byte>,
                                       bool) {
  if (!session) {
    session = std::make_shared<Session>(stats);
    session->set_PrepareResponse_callback(std::move(provider));
    connection.set_HandleWriteComplete_callback(
        std::bind_front(&Session::HandleWriteComplete, session));
  }
  if (session->stats) ++session->stats->callbacks;
  session->HandleMessage(connection);
}

net::TcpConnection::MessageCallback HttpMessageFactory::CreateMessageCallback()
    const {
  HttpMessageHandler handler;
  handler.set_PrepareResponse_callback(
      std::bind_front(&http::StaticFileService::PrepareResponse, &service));
  return std::bind_front(&HttpMessageHandler::HandleMessage,
                         std::move(handler));
}

// Narrow test-only observation seam; no scheduling or product behavior is
// injected.
void set_RecordSessionEvent_callback(void (*observer)(bool,
                                                      const void*) noexcept) {
  session_observer.store(observer);
}

net::TcpConnection::MessageCallback MakeHttpCallback(ResponseProvider provider,
                                                     HttpCallbackStats* stats) {
  // Mutable Session remains lazy: creation occurs on the connection IO owner.
  HttpMessageHandler handler;
  handler.stats = stats;
  handler.set_PrepareResponse_callback(std::move(provider));
  return std::bind_front(&HttpMessageHandler::HandleMessage,
                         std::move(handler));
}

net::TcpServer::MessageCallbackFactory MakeHttpFactory(
    const http::StaticFileService& service) {
  return std::bind_front(&HttpMessageFactory::CreateMessageCallback,
                         HttpMessageFactory{service});
}

}  // namespace hp::app
