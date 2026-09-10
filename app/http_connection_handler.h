#pragma once
#include "http/static_file_service.h"
#include "net/tcp_server.h"

namespace hp::app {
struct HttpCallbackStats {
  std::size_t callbacks{}, parses{}, need_more{}, responses{},
      eof_notifications{};
  std::size_t submitted_bytes{}, accepted_bytes{}, consumed_bytes{};
};

using ResponseProvider = std::function<http::ResponseResult(
    const http::HttpRequest&, http::ConnectionPolicy)>;

// Each callback lazily creates its Session on the connection owner. Shared
// custom provider/stats require caller synchronization; service outlives all
// workers.
net::TcpConnection::MessageCallback make_http_callback(
    ResponseProvider provider, HttpCallbackStats* stats = nullptr);
net::TcpServer::MessageCallbackFactory make_http_factory(
    const http::StaticFileService& service);
}  // namespace hp::app
