#pragma once
#include "http/static_file_service.h"
#include "net/tcp_server.h"

namespace hp::app {
struct HttpCallbackStats {
  std::size_t callbacks{}, parses{}, need_more{}, responses{},
      eof_notifications{};
  std::size_t submitted_bytes{}, accepted_bytes{}, consumed_bytes{};
};

using ResponseProvider =
    std::function<http::ResponseResult(const http::HttpRequest&,
                                       http::ConnectionPolicy)>;

// Test observation hook. Install before session creation; clear after owners
// stop.
void set_RecordSessionEvent_callback(void (*observer)(bool,
                                                      const void*) noexcept);

// Response strategies are registered explicitly; Session stays lazy and bound
// to its original IO owner. Copies before first dispatch retain independent
// state.
struct Session {
  enum class Phase { kReading, kWriting, kClosing };
  ResponseProvider provider;
  HttpCallbackStats* stats;
  http::RequestParser parser;
  Phase phase{Phase::kReading};
  bool completed{false}, close{false};

  explicit Session(HttpCallbackStats* callback_stats);
  ~Session();
  void set_PrepareResponse_callback(ResponseProvider response_provider);
  void HandleMessage(net::TcpConnection& connection);
  void HandleWriteComplete(net::TcpConnection& connection);
};

struct HttpMessageHandler {
  ResponseProvider provider;
  HttpCallbackStats* stats{};
  std::shared_ptr<Session> session;

  void set_PrepareResponse_callback(ResponseProvider response_provider);
  void HandleMessage(net::TcpConnection& connection,
                     std::span<const std::byte>,
                     bool);
};

struct HttpMessageFactory {
  const http::StaticFileService& service;
  net::TcpConnection::MessageCallback CreateMessageCallback() const;
};

// Each callback lazily creates its Session on the connection owner. Shared
// custom provider/stats require caller synchronization; service outlives all
// workers.
net::TcpConnection::MessageCallback MakeHttpCallback(
    ResponseProvider provider,
    HttpCallbackStats* stats = nullptr);
net::TcpServer::MessageCallbackFactory MakeHttpFactory(
    const http::StaticFileService& service);
}  // namespace hp::app
