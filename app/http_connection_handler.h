#pragma once
#include "net/tcp_server.h"
#include "http/static_file_service.h"

namespace hp::app {
struct HttpCallbackStats {
    std::size_t callbacks{}, parses{}, need_more{}, responses{}, eof_notifications{};
    std::size_t submitted_bytes{}, accepted_bytes{}, consumed_bytes{};
};
using ResponseProvider = std::function<http::ResponseResult(const http::HttpRequest&, http::ConnectionPolicy)>;
// Each call creates independent serial session state. Provider/service outlives the connection.
net::TcpConnection::MessageCallback make_http_callback(ResponseProvider provider,
                                                      HttpCallbackStats* stats = nullptr);
net::TcpServer::MessageCallbackFactory make_http_factory(const http::StaticFileService& service);
}
