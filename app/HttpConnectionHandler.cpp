#include "HttpConnectionHandler.h"

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "base/Logger.h"
#include "http/HttpResponse.h"

namespace hp::app {
Session::Session(HttpCallbackStats* callbackStats,
                 metrics::ServerMetrics* serverMetrics,
                 bool enableAccessLog)
    : stats(callbackStats), metrics(serverMetrics), accessLogEnabled(enableAccessLog) {
  if (auto observer = sessionEventCallback.load()) observer(true, this);
}

Session::~Session() {
  finishPendingRequest(true);
  if (auto observer = sessionEventCallback.load()) observer(false, this);
}

void Session::finishPendingRequest(bool aborted) noexcept {
  if (!requestPending) return;
  requestPending = false;
  accessRecord.aborted = aborted;
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - requestStarted)
                           .count();
  accessRecord.durationUs = elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
  if (metrics) metrics->finishRequest(aborted, accessRecord.status, accessRecord.durationUs);
  if (accessLogEnabled) submitAccessRecord(accessRecord, metrics);
}

void Session::onMessage(net::TcpConnection& connection) {
  if (phase != Phase::kReading || connection.isDraining()) return;
  const auto input = connection.inputView();
  const bool eof = connection.peerClosed();
  if (!requestPending && !input.empty()) {
    requestPending = true;
    requestStarted = std::chrono::steady_clock::now();
    accessRecord = {};
    if (metrics) metrics->beginRequest();
  }
  if (stats) {
    ++stats->parses;
    if (eof) ++stats->eofNotifications;
  }
  const auto parsed =
      parser.feedRequestBytes({reinterpret_cast<const char*>(input.data()), input.size()});
  if (stats) {
    stats->submittedBytes += input.size();
    stats->acceptedBytes += parsed.acceptedBytes;
  }
  if (accessLogEnabled) accessRecord.copyRequest(parsed.request.method, parsed.request.target);
  connection.consumeInputBytes(parsed.acceptedBytes);  // 不得再次使用借用的输入。
  if (stats) stats->consumedBytes += parsed.acceptedBytes;
  // 请求尚未收完整
  if (parsed.status == http::ParseStatus::kNeedMore && !eof) {
    if (stats) ++stats->needMore;
    connection.setIdleWait(completed && parsed.requestBytes == 0 && connection.pendingBytes() == 0);
    connection.resumeReading();
    return;
  }
  if (parsed.status == http::ParseStatus::kNeedMore && completed && parsed.requestBytes == 0) {
    phase = Phase::kClosing;
    connection.closeAfterFlush();
    return;
  }
  // 可能设置 kClose，后续构造 400 响应
  close = parsed.status != http::ParseStatus::kComplete || parsed.request.closeRequested;
  const auto policy = close ? http::ConnectionPolicy::kClose : http::ConnectionPolicy::kKeepAlive;
  connection.setIdleWait(false);
  // 会话进入响应阶段，暂停连接读取输入缓冲区
  phase = Phase::kWriting;
  connection.pauseReading();
  std::vector<std::byte> response;
  std::optional<base::FileRegion> file;
  try {
    switch (parsed.status) {
      case http::ParseStatus::kNeedMore:
      case http::ParseStatus::kBadRequest:
        accessRecord.status = static_cast<int>(http::Status::kBadRequest);
        accessRecord.contentBytes = http::errorContentBytes(http::Status::kBadRequest);
        if (metrics && requestPending) metrics->recordParseError();
        response = http::makeErrorResponse(http::Status::kBadRequest, policy);
        break;
      case http::ParseStatus::kMethodNotAllowed:
        accessRecord.status = static_cast<int>(http::Status::kMethodNotAllowed);
        accessRecord.contentBytes = http::errorContentBytes(http::Status::kMethodNotAllowed);
        if (metrics && requestPending) metrics->recordParseError();
        response = http::makeErrorResponse(http::Status::kMethodNotAllowed, policy);
        break;
      case http::ParseStatus::kComplete: {
        // 调用响应回调 responseCallback()
        auto result = responseCallback(parsed.request, policy);
        if (close && result.effectivePolicy != http::ConnectionPolicy::kClose)
          throw std::logic_error("provider relaxed terminal connection policy");
        accessRecord.status = static_cast<int>(result.status);
        accessRecord.contentBytes = result.contentBytes;
        close = result.effectivePolicy == http::ConnectionPolicy::kClose;
        response = std::move(result.bytes);
        file = std::move(result.file);  // 仅当成功构造文件响应时，`file` 才有值
        break;
      }
    }
  } catch (...) {
    if (metrics) metrics->recordProviderError();
    accessRecord.status = static_cast<int>(http::Status::kInternalServerError);
    accessRecord.contentBytes = http::errorContentBytes(http::Status::kInternalServerError);
    close = true;
    file.reset();
    response =
        http::makeErrorResponse(http::Status::kInternalServerError, http::ConnectionPolicy::kClose);
  }
  // send 可同步触发完成并递归处理pipeline，所有状态须先写定。
  completed = true;
  if (stats) ++stats->responses;
  if (file)
    connection.sendFile(response, std::move(*file));
  else
    connection.sendBytes(response);
}

void Session::onWriteComplete(net::TcpConnection& connection) {
  if (phase != Phase::kWriting) return;
  finishPendingRequest(false);
  if (close || connection.isDraining()) {
    phase = Phase::kClosing;
    connection.closeAfterFlush();
    return;
  }
  parser.resetRequestParser();  // reset() 之后才能开始下一请求
  phase = Phase::kReading;
  // 即使没有新的可读事件，也处理传输层持有的剩余输入。
  onMessage(connection);
}

void HttpMessageHandler::onMessage(net::TcpConnection& connection,
                                   std::span<const std::byte>,
                                   bool) {
  if (!session) {
    session = std::make_shared<Session>(stats, metrics, accessLogEnabled);
    session->setResponseCallback(std::move(responseCallback));
    connection.setWriteCompleteCallback([session = session](net::TcpConnection& connection) {
      session->onWriteComplete(connection);
    });
  }
  if (session->stats) ++session->stats->callbacks;
  session->onMessage(connection);
}

net::TcpConnection::MessageCallback HttpMessageFactory::onMessageFactory() const {
  HttpMessageHandler handler;
  handler.metrics = metrics;
  handler.accessLogEnabled = accessLogEnabled;
  handler.setResponseCallback(
      [target = &service](const http::HttpRequest& request, http::ConnectionPolicy policy) {
        return target->onResponse(request, policy);
      });
  return
      [target = std::move(handler)](net::TcpConnection& connection,
                                    std::span<const std::byte> input,
                                    bool eof) mutable { target.onMessage(connection, input, eof); };
}

net::TcpConnection::MessageCallback makeHttpCallback(ResponseCallback responseCallback,
                                                     HttpCallbackStats* stats,
                                                     metrics::ServerMetrics* metrics,
                                                     bool accessLogEnabled) {
  // 可变的 Session 仍延迟创建：创建发生在连接所属的 IO 线程。
  HttpMessageHandler handler;
  handler.stats = stats;
  handler.metrics = metrics;
  handler.accessLogEnabled = accessLogEnabled;
  handler.setResponseCallback(std::move(responseCallback));
  return
      [target = std::move(handler)](net::TcpConnection& connection,
                                    std::span<const std::byte> input,
                                    bool eof) mutable { target.onMessage(connection, input, eof); };
}

net::TcpServer::MessageFactoryCallback makeHttpFactory(const http::StaticFileService& service) {
  return [target = HttpMessageFactory{service}]() { return target.onMessageFactory(); };
}

}  // namespace hp::app
