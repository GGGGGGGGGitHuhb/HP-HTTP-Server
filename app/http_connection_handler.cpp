#include "http_connection_handler.h"
#include "http/http_response.h"
#include "base/logger.h"
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace hp::app {
namespace {
struct Session {
    enum class Phase { reading, writing, closing };
    ResponseProvider provider;
    HttpCallbackStats* stats;
    http::RequestParser parser;
    Phase phase{Phase::reading};
    bool completed{false}, close{false};

    void advance(net::TcpConnection& connection) {
        if (phase != Phase::reading)
            return;
        const auto input = connection.input_view();
        const bool eof = connection.peer_closed();
        if (stats) {
            ++stats->parses;
            if (eof)
                ++stats->eof_notifications;
        }
        const auto parsed =
            parser.feed({reinterpret_cast<const char*>(input.data()), input.size()});
        if (stats) {
            stats->submitted_bytes += input.size();
            stats->accepted_bytes += parsed.accepted_bytes;
        }
        connection.consume(parsed.accepted_bytes); // Never use the borrowed input again.
        if (stats)
            stats->consumed_bytes += parsed.accepted_bytes;
        if (parsed.status == http::ParseStatus::need_more && !eof) {
            if (stats)
                ++stats->need_more;
            connection.resume_reading();
            return;
        }
        if (parsed.status == http::ParseStatus::need_more && completed &&
            parsed.request_bytes == 0) {
            phase = Phase::closing;
            connection.close_after_flush();
            return;
        }
        close = parsed.status != http::ParseStatus::complete || parsed.request.close_requested;
        const auto policy =
            close ? http::ConnectionPolicy::close : http::ConnectionPolicy::keep_alive;
        phase = Phase::writing;
        connection.pause_reading();
        std::vector<std::byte> response;
        try {
            switch (parsed.status) {
            case http::ParseStatus::need_more:
            case http::ParseStatus::bad_request:
                response = http::make_error_response(http::Status::bad_request, policy);
                break;
            case http::ParseStatus::method_not_allowed:
                response = http::make_error_response(http::Status::method_not_allowed, policy);
                break;
            case http::ParseStatus::complete: {
                auto result = provider(parsed.request, policy);
                if (close && result.effective_policy != http::ConnectionPolicy::close)
                    throw std::logic_error("provider relaxed terminal connection policy");
                close = result.effective_policy == http::ConnectionPolicy::close;
                response = std::move(result.bytes);
                break;
            }
            }
        } catch (...) {
            close = true;
            response = http::make_error_response(http::Status::internal_server_error,
                                                 http::ConnectionPolicy::close);
        }
        connection.send(response);
        completed = true;
        if (stats)
            ++stats->responses;
        base::info(
            "S3 evidence: HTTP message callback produced one response via incremental parser.");
    }

    void drained(net::TcpConnection& connection) {
        if (phase != Phase::writing)
            return;
        if (close) {
            phase = Phase::closing;
            connection.close_after_flush();
            return;
        }
        parser.reset();
        phase = Phase::reading;
        // Process transport-owned suffix even without a new readable event.
        advance(connection);
    }
};
}

net::TcpConnection::MessageCallback make_http_callback(ResponseProvider provider,
                                                       HttpCallbackStats* stats) {
    auto session = std::make_shared<Session>(Session{std::move(provider), stats, {}});
    return [session, installed = false](net::TcpConnection& connection, std::span<const std::byte>,
                                        bool) mutable {
        if (!installed) {
            connection.set_write_complete_callback([session](net::TcpConnection& c) {
                session->drained(c);
            });
            installed = true;
        }
        if (session->stats)
            ++session->stats->callbacks;
        session->advance(connection);
    };
}

net::TcpServer::MessageCallbackFactory make_http_factory(const http::StaticFileService& service) {
    return [&service] {
        return make_http_callback(
            [&service](const http::HttpRequest& request, http::ConnectionPolicy policy) {
                return service.handle_response(request, policy);
            });
    };
}
}
