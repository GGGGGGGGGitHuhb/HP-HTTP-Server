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
                 bool enableAccessLog,
                 std::chrono::milliseconds readTimeout,
                 std::shared_ptr<base::ResourceBudget> budget)
    : stats(callbackStats),
      metrics(serverMetrics),
      accessLogEnabled(enableAccessLog),
      requestReadTimeout(readTimeout),
      resourceBudget(std::move(budget)) {
  if (auto observer = sessionEventCallback.load()) observer(true, this);
}

Session::~Session() {
  if (pendingJob) asyncHandler->cancelRequest(pendingJob);
  finishPendingRequest(true);
  if (auto observer = sessionEventCallback.load()) observer(false, this);
}

void Session::finishPendingRequest(bool aborted) noexcept {
  requestLease.reset();
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

void Session::closeResourceRejected(net::TcpConnection& connection) {
  phase = Phase::kClosing;
  completed = false;
  accessRecord.status = 0;
  accessRecord.contentBytes = 0;
  clearReadDeadline(connection);
  finishPendingRequest(true);
  connection.requestClose();
}

bool Session::closeExpiredRequest(net::TcpConnection& connection) {
  if (!readDeadline || std::chrono::steady_clock::now() < *readDeadline) return false;
  phase = Phase::kClosing;
  clearReadDeadline(connection);
  connection.requestClose();
  return true;
}

void Session::clearReadDeadline(net::TcpConnection& connection) {
  readDeadline.reset();
  connection.setReadDeadline(std::nullopt);
}

void Session::onMessage(net::TcpConnection& connection) {
  if (phase != Phase::kReading || connection.isDraining()) return;
  if (closeExpiredRequest(connection)) return;
  const auto input = connection.inputView();
  const bool eof = connection.peerClosed();
  if (!requestPending && !input.empty()) {
    requestPending = true;
    requestStarted = std::chrono::steady_clock::now();
    accessRecord = {};
    if (metrics) metrics->beginRequest();
    if (resourceBudget) {
      auto acquired = resourceBudget->tryAcquireInput(base::kRequestInputBytes);
      if (!acquired.accepted()) {
        closeResourceRejected(connection);
        return;
      }
      requestLease = std::move(acquired.lease);
    }
    if (requestReadTimeout.count() > 0) {
      readDeadline = requestStarted + requestReadTimeout;
      connection.setReadDeadline(readDeadline);
    }
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
  if (closeExpiredRequest(connection)) return;
  if (parsed.status == http::ParseStatus::kHeadersComplete) {
    onRequestHeaders(connection, *parsed.head);
    return;
  }
  clearReadDeadline(connection);
  // 可能设置 kClose，后续构造 400 响应
  close = parsed.status != http::ParseStatus::kComplete || parsed.request.closeRequested;
  const auto policy = close ? http::ConnectionPolicy::kClose : http::ConnectionPolicy::kKeepAlive;
  connection.setIdleWait(false);
  // 会话进入响应阶段，暂停连接读取输入缓冲区
  phase = Phase::kWriting;
  connection.pauseReading();
  const bool omitBody = parser.isHeadRequest();
  std::vector<std::byte> response;
  std::optional<base::FileRegion> file;
  try {
    switch (parsed.status) {
      case http::ParseStatus::kNeedMore:
      case http::ParseStatus::kBadRequest:
        accessRecord.status = static_cast<int>(http::Status::kBadRequest);
        accessRecord.contentBytes =
            omitBody ? 0 : http::errorContentBytes(http::Status::kBadRequest);
        if (metrics && requestPending) metrics->recordParseError();
        response = http::makeErrorResponse(http::Status::kBadRequest, policy, omitBody);
        break;
      case http::ParseStatus::kPayloadTooLarge:
        accessRecord.status = static_cast<int>(http::Status::kContentTooLarge);
        accessRecord.contentBytes =
            omitBody ? 0 : http::errorContentBytes(http::Status::kContentTooLarge);
        if (metrics && requestPending) metrics->recordParseError();
        response = http::makeErrorResponse(http::Status::kContentTooLarge, policy, omitBody);
        break;
      case http::ParseStatus::kExpectationFailed:
      case http::ParseStatus::kNotImplemented: {
        const auto status = parsed.status == http::ParseStatus::kExpectationFailed
                                ? http::Status::kExpectationFailed
                                : http::Status::kNotImplemented;
        accessRecord.status = static_cast<int>(status);
        accessRecord.contentBytes = omitBody ? 0 : http::errorContentBytes(status);
        if (metrics && requestPending) metrics->recordParseError();
        response = http::makeErrorResponse(status, policy, omitBody);
        break;
      }
      case http::ParseStatus::kHeadersComplete:
      case http::ParseStatus::kMethodNotAllowed:
        throw std::logic_error("parser returned obsolete method status");
      case http::ParseStatus::kComplete: {
        if (asyncHandler) {
          auto submission = asyncHandler->submitRequest(parsed.request, policy);
          if (submission.dispatch == AsyncDispatch::kPending) {
            pendingJob = std::move(submission.job);
            ++taskGeneration;
            phase = Phase::kAwaitingTask;
            try {
              armTaskContinuation(connection);
            } catch (...) {
              asyncHandler->cancelRequest(pendingJob);
              pendingJob.reset();
              connection.cancelContinuation();
              sendFinalResponse(connection, AsyncTaskHandler::failureResponse());
            }
            return;
          }
          if (submission.response) {
            sendFinalResponse(connection, std::move(*submission.response));
            return;
          }
        }
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
    accessRecord.contentBytes =
        omitBody ? 0 : http::errorContentBytes(http::Status::kInternalServerError);
    close = true;
    file.reset();
    response = http::makeErrorResponse(http::Status::kInternalServerError,
                                       http::ConnectionPolicy::kClose,
                                       omitBody);
  }
  sendFinalResponse(connection,
                    {std::move(response),
                     close ? http::ConnectionPolicy::kClose : http::ConnectionPolicy::kKeepAlive,
                     std::move(file),
                     static_cast<http::Status>(accessRecord.status),
                     accessRecord.contentBytes});
}

void Session::sendFinalResponse(net::TcpConnection& connection, http::ResponseResult result) {
  phase = Phase::kWriting;
  close = result.effectivePolicy == http::ConnectionPolicy::kClose;
  accessRecord.status = static_cast<int>(result.status);
  accessRecord.contentBytes = result.contentBytes;
  // send 可同步触发完成并递归处理pipeline，所有状态须先写定。
  completed = true;
  if (stats) ++stats->responses;
  try {
    if (result.file)
      connection.sendFile(result.bytes, std::move(*result.file));
    else
      connection.sendBytes(result.bytes);
  } catch (const base::ResourceLimitError&) {
    if (stats) --stats->responses;
    closeResourceRejected(connection);
  }
}

void Session::armTaskContinuation(net::TcpConnection& connection) {
  const auto next = std::min(std::chrono::steady_clock::now() + std::chrono::milliseconds{5},
                             pendingJob->deadline());
  connection.scheduleContinuation(
      next,
      [weak = weak_from_this(), generation = taskGeneration](net::TcpConnection& target) {
        if (auto session = weak.lock()) session->onTaskContinuation(target, generation);
      });
}

void Session::onTaskContinuation(net::TcpConnection& connection, std::uint64_t generation) {
  if (phase != Phase::kAwaitingTask || generation != taskGeneration || connection.isDraining())
    return;
  std::optional<http::ResponseResult> result;
  try {
    if (std::chrono::steady_clock::now() < pendingJob->deadline())
      result = asyncHandler->collectResponse(pendingJob);
    if (std::chrono::steady_clock::now() >= pendingJob->deadline())
      result = AsyncTaskHandler::timeoutResponse();
    if (!result) {
      armTaskContinuation(connection);
      return;
    }
  } catch (...) {
    result = AsyncTaskHandler::failureResponse();
  }
  asyncHandler->cancelRequest(pendingJob);
  pendingJob.reset();
  connection.cancelContinuation();
  sendFinalResponse(connection, std::move(*result));
}

void Session::onRequestHeaders(net::TcpConnection& connection, const http::RequestHead& head) {
  if (accessLogEnabled) accessRecord.copyRequest(head.method, head.target);
  std::optional<http::ResponseResult> rejected;
  try {
    const auto policy =
        head.closeRequested ? http::ConnectionPolicy::kClose : http::ConnectionPolicy::kKeepAlive;
    if (requestHeadersCallback) rejected = requestHeadersCallback(head, policy);
    if (rejected && rejected->effectivePolicy != http::ConnectionPolicy::kClose)
      throw std::logic_error("header rejection relaxed terminal connection policy");
  } catch (...) {
    if (metrics) metrics->recordProviderError();
    const bool omitBody = parser.isHeadRequest();
    rejected = http::ResponseResult{
        http::makeErrorResponse(http::Status::kInternalServerError,
                                http::ConnectionPolicy::kClose,
                                omitBody),
        http::ConnectionPolicy::kClose,
        {},
        http::Status::kInternalServerError,
        omitBody ? 0 : http::errorContentBytes(http::Status::kInternalServerError)};
  }
  if (closeExpiredRequest(connection)) return;
  connection.setIdleWait(false);
  connection.pauseReading();
  if (!rejected) {
    phase = Phase::kInterimWriting;
    try {
      connection.sendBytes(http::makeContinueResponse());
    } catch (const base::ResourceLimitError&) {
      closeResourceRejected(connection);
    }
    return;
  }
  clearReadDeadline(connection);
  sendFinalResponse(connection, std::move(*rejected));
}

void Session::onWriteComplete(net::TcpConnection& connection) {
  if (phase == Phase::kInterimWriting) {
    if (connection.isDraining()) {
      phase = Phase::kClosing;
      clearReadDeadline(connection);
      finishPendingRequest(true);
      connection.closeAfterFlush();
      return;
    }
    if (closeExpiredRequest(connection)) return;
    phase = Phase::kReading;
    onMessage(connection);
    return;
  }
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
    session = std::make_shared<Session>(stats,
                                        metrics,
                                        accessLogEnabled,
                                        requestReadTimeout,
                                        resourceBudget);
    session->asyncHandler = asyncHandler;
    session->setResponseCallback(std::move(responseCallback));
    session->setRequestHeadersCallback(std::move(requestHeadersCallback));
    connection.setWriteCompleteCallback([session = session](net::TcpConnection& connection) {
      session->onWriteComplete(connection);
    });
  }
  if (session->stats) ++session->stats->callbacks;
  session->onMessage(connection);
}

net::TcpConnection::MessageCallback HttpMessageFactory::onMessageFactory() const {
  HttpMessageHandler handler;
  handler.asyncHandler = asyncHandler;
  handler.resourceBudget = resourceBudget;
  handler.metrics = metrics;
  handler.accessLogEnabled = accessLogEnabled;
  handler.requestReadTimeout = requestReadTimeout;
  handler.setRequestHeadersCallback(
      [target = &router, owner = ownedRouter](const http::RequestHead& head,
                                              http::ConnectionPolicy policy) {
        return target->onRequestHeaders(head, policy);
      });
  handler.setResponseCallback(
      [target = &router, owner = ownedRouter](const http::HttpRequest& request,
                                              http::ConnectionPolicy policy) {
        return target->onResponse(request, policy);
      });
  return
      [target = std::move(handler)](net::TcpConnection& connection,
                                    std::span<const std::byte> input,
                                    bool eof) mutable { target.onMessage(connection, input, eof); };
}

net::TcpConnection::MessageCallback makeHttpCallback(
    ResponseCallback responseCallback,
    HttpCallbackStats* stats,
    metrics::ServerMetrics* metrics,
    bool accessLogEnabled,
    std::chrono::milliseconds requestReadTimeout,
    std::shared_ptr<base::ResourceBudget> resourceBudget) {
  // 可变的 Session 仍延迟创建：创建发生在连接所属的 IO 线程。
  HttpMessageHandler handler;
  handler.stats = stats;
  handler.resourceBudget = std::move(resourceBudget);
  handler.metrics = metrics;
  handler.accessLogEnabled = accessLogEnabled;
  handler.requestReadTimeout = requestReadTimeout;
  handler.setResponseCallback(std::move(responseCallback));
  return
      [target = std::move(handler)](net::TcpConnection& connection,
                                    std::span<const std::byte> input,
                                    bool eof) mutable { target.onMessage(connection, input, eof); };
}

net::TcpServer::MessageFactoryCallback makeHttpFactory(
    const http::StaticFileService& service,
    std::chrono::milliseconds requestReadTimeout,
    std::shared_ptr<base::ResourceBudget> resourceBudget) {
  // 兼容静态入口：工厂拥有 router，service 仍须覆盖所有 worker 的生命周期。
  auto router = std::make_shared<http::RequestRouter>(service);
  return [target = HttpMessageFactory{*router,
                                      nullptr,
                                      false,
                                      requestReadTimeout,
                                      router,
                                      {},
                                      std::move(resourceBudget)}]() {
    return target.onMessageFactory();
  };
}

}  // namespace hp::app
