#include "HttpConnectionHandler.h"

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "base/Logger.h"
#include "http/HttpResponse.h"

namespace hp::app {
Session::Session(HttpCallbackStats* callbackStats) : stats(callbackStats) {
  if (auto observer = sessionEventCallback.load()) observer(true, this);
}

Session::~Session() {
  if (auto observer = sessionEventCallback.load()) observer(false, this);
}

void Session::onMessage(net::TcpConnection& connection) {
  if (phase != Phase::kReading) return;
  const auto input = connection.inputView();
  const bool eof = connection.peerClosed();
  if (stats) {
    ++stats->parses;
    if (eof) ++stats->eofNotifications;
  }
  const auto parsed =
      parser.feed({reinterpret_cast<const char*>(input.data()), input.size()});
  if (stats) {
    stats->submittedBytes += input.size();
    stats->acceptedBytes += parsed.acceptedBytes;
  }
  connection.consume(
      parsed.acceptedBytes);  // Never use the borrowed input again.
  if (stats) stats->consumedBytes += parsed.acceptedBytes;
  if (parsed.status == http::ParseStatus::kNeedMore && !eof) {
    if (stats) ++stats->needMore;
    connection.setIdleWait(completed && parsed.requestBytes == 0 &&
                           connection.pendingBytes() == 0);
    connection.resumeReading();
    return;
  }
  if (parsed.status == http::ParseStatus::kNeedMore && completed &&
      parsed.requestBytes == 0) {
    phase = Phase::kClosing;
    connection.closeAfterFlush();
    return;
  }
  close = parsed.status != http::ParseStatus::kComplete ||
          parsed.request.closeRequested;
  const auto policy = close ? http::ConnectionPolicy::kClose
                            : http::ConnectionPolicy::kKeepAlive;
  connection.setIdleWait(false);
  phase = Phase::kWriting;
  connection.pauseReading();
  std::vector<std::byte> response;
  std::optional<base::FileRegion> file;
  try {
    switch (parsed.status) {
      case http::ParseStatus::kNeedMore:
      case http::ParseStatus::kBadRequest:
        response = http::makeErrorResponse(http::Status::kBadRequest, policy);
        break;
      case http::ParseStatus::kMethodNotAllowed:
        response =
            http::makeErrorResponse(http::Status::kMethodNotAllowed, policy);
        break;
      case http::ParseStatus::kComplete: {
        auto result = responseCallback(parsed.request, policy);
        if (close && result.effectivePolicy != http::ConnectionPolicy::kClose)
          throw std::logic_error("provider relaxed terminal connection policy");
        close = result.effectivePolicy == http::ConnectionPolicy::kClose;
        response = std::move(result.bytes);
        file = std::move(result.file);
        break;
      }
    }
  } catch (...) {
    close = true;
    file.reset();
    response = http::makeErrorResponse(http::Status::kInternalServerError,
                                       http::ConnectionPolicy::kClose);
  }
  if (file)
    connection.sendFile(response, std::move(*file));
  else
    connection.send(response);
  completed = true;
  if (stats) ++stats->responses;
  base::info(
      "S3 evidence: HTTP message callback produced one response via "
      "incremental parser.");
}

void Session::onWriteComplete(net::TcpConnection& connection) {
  if (phase != Phase::kWriting) return;
  if (close) {
    phase = Phase::kClosing;
    connection.closeAfterFlush();
    return;
  }
  parser.reset();
  phase = Phase::kReading;
  // Process transport-owned suffix even without a new readable event.
  onMessage(connection);
}

void HttpMessageHandler::onMessage(net::TcpConnection& connection,
                                   std::span<const std::byte>,
                                   bool) {
  if (!session) {
    session = std::make_shared<Session>(stats);
    session->setResponseCallback(std::move(responseCallback));
    connection.setWriteCompleteCallback(
        [session = session](net::TcpConnection& connection) {
          session->onWriteComplete(connection);
        });
  }
  if (session->stats) ++session->stats->callbacks;
  session->onMessage(connection);
}

net::TcpConnection::MessageCallback HttpMessageFactory::onMessageFactory()
    const {
  HttpMessageHandler handler;
  handler.setResponseCallback(
      [target = &service](const http::HttpRequest& request,
                          http::ConnectionPolicy policy) {
        return target->onResponse(request, policy);
      });
  return [target = std::move(handler)](net::TcpConnection& connection,
                                       std::span<const std::byte> input,
                                       bool eof) mutable {
    target.onMessage(connection, input, eof);
  };
}

net::TcpConnection::MessageCallback makeHttpCallback(
    ResponseCallback responseCallback,
    HttpCallbackStats* stats) {
  // Mutable Session remains lazy: creation occurs on the connection IO owner.
  HttpMessageHandler handler;
  handler.stats = stats;
  handler.setResponseCallback(std::move(responseCallback));
  return [target = std::move(handler)](net::TcpConnection& connection,
                                       std::span<const std::byte> input,
                                       bool eof) mutable {
    target.onMessage(connection, input, eof);
  };
}

net::TcpServer::MessageFactoryCallback makeHttpFactory(
    const http::StaticFileService& service) {
  return [target = HttpMessageFactory{service}]() {
    return target.onMessageFactory();
  };
}

}  // namespace hp::app
