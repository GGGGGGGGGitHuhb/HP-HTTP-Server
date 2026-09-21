#pragma once
#include <atomic>

#include "http/StaticFileService.h"
#include "net/TcpServer.h"

namespace hp::app {
struct HttpCallbackStats {
  std::size_t callbacks{}, parses{}, needMore{}, responses{},
      eofNotifications{};
  std::size_t submittedBytes{}, acceptedBytes{}, consumedBytes{};
};

using ResponseCallback =
    std::function<http::ResponseResult(const http::HttpRequest&,
                                       http::ConnectionPolicy)>;

// Test observation hook. Install before session creation; clear after owners
// stop.
using SessionEventCallback = void (*)(bool, const void*) noexcept;
inline std::atomic<SessionEventCallback> sessionEventCallback{nullptr};
inline void setSessionEventCallback(
    SessionEventCallback newSessionEventCallback) {
  sessionEventCallback.store(newSessionEventCallback);
}

// Response strategies are registered explicitly; Session stays lazy and bound
// to its original IO owner. Copies before first dispatch retain independent
// state.
struct Session {
  enum class Phase { kReading, kWriting, kClosing };
  ResponseCallback responseCallback;
  HttpCallbackStats* stats;
  http::RequestParser parser;
  Phase phase{Phase::kReading};
  bool completed{false}, close{false};

  explicit Session(HttpCallbackStats* callbackStats);
  ~Session();
  void setResponseCallback(ResponseCallback responseCallback) {
    this->responseCallback = std::move(responseCallback);
  }

  void onMessage(net::TcpConnection& connection);
  void onWriteComplete(net::TcpConnection& connection);
};

struct HttpMessageHandler {
  ResponseCallback responseCallback;
  HttpCallbackStats* stats{};
  std::shared_ptr<Session> session;

  void setResponseCallback(ResponseCallback responseCallback) {
    this->responseCallback = std::move(responseCallback);
  }

  void onMessage(net::TcpConnection& connection,
                 std::span<const std::byte>,
                 bool);
};

struct HttpMessageFactory {
  const http::StaticFileService& service;
  net::TcpConnection::MessageCallback onMessageFactory() const;
};

// Each callback lazily creates its Session on the connection owner. Shared
// custom provider/stats require caller synchronization; service outlives all
// workers.
net::TcpConnection::MessageCallback makeHttpCallback(
    ResponseCallback responseCallback,
    HttpCallbackStats* stats = nullptr);
net::TcpServer::MessageFactoryCallback makeHttpFactory(
    const http::StaticFileService& service);
}  // namespace hp::app
