#include "http_connection_handler.h"
#include "http/http_response.h"
#include "base/logger.h"
#include <string_view>
#include <utility>

namespace hp::app {
net::TcpConnection::MessageCallback make_http_callback(ResponseProvider provider, HttpCallbackStats* stats) {
    return [provider = std::move(provider), stats, done = false](net::TcpConnection& connection,
            std::span<const std::byte> input, bool peer_closed) mutable {
        if (done) return;
        if (stats) { ++stats->callbacks; ++stats->parses; if (peer_closed) ++stats->eof_notifications; }
        std::vector<std::byte> response;
        try {
            const auto parsed = http::parse_request({reinterpret_cast<const char*>(input.data()), input.size()});
            switch (parsed.status) {
                case http::ParseStatus::need_more:
                    if (!peer_closed) { if (stats) ++stats->need_more; return; }
                    response = http::make_error_response(http::Status::bad_request); break;
                case http::ParseStatus::bad_request:
                    response = http::make_error_response(http::Status::bad_request); break;
                case http::ParseStatus::method_not_allowed:
                    response = http::make_error_response(http::Status::method_not_allowed); break;
                case http::ParseStatus::complete:
                    response = provider(parsed.request); break;
            }
        } catch (...) {
            response = http::make_error_response(http::Status::internal_server_error);
        }
        done = true;
        const auto consumed = input.size();
        connection.send(response);
        connection.consume(consumed);
        connection.close_after_flush();
        if (stats) ++stats->responses;
        base::info("S3 evidence: HTTP message callback produced one response.");
    };
}
net::TcpServer::MessageCallbackFactory make_http_factory(const http::StaticFileService& service) {
    return [&service] { return make_http_callback([&service](const http::HttpRequest& request) {
        return service.handle(request);
    }); };
}
}
