#include "http_connection_handler.h"
#include "http/http_response.h"
#include "base/logger.h"
#include <memory>
#include <atomic>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace hp::app {
namespace {
using SessionObserver = void (*)(bool, const void*) noexcept;
std::atomic<SessionObserver> session_observer{nullptr};

struct Session {
    enum class Phase { reading, writing, closing };
    ResponseProvider provider;
    HttpCallbackStats* stats;
    http::RequestParser parser;
    Phase phase{Phase::reading};
    bool completed{false}, close{false};

    Session(ResponseProvider response_provider, HttpCallbackStats* callback_stats)
        : provider(std::move(response_provider)), stats(callback_stats) {
        if (auto observer = session_observer.load())
            observer(true, this);
    }

    ~Session() {
        if (auto observer = session_observer.load())
            observer(false, this);
    }

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
            connection.set_idle_wait(completed && parsed.request_bytes == 0 &&
                                     connection.pending_bytes() == 0);
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
        connection.set_idle_wait(false);
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
} // namespace

// Narrow test-only observation seam; no scheduling or product behavior is injected.
void set_session_observer_for_test(void (*observer)(bool, const void*) noexcept) {
    session_observer.store(observer);
}

net::TcpConnection::MessageCallback make_http_callback(ResponseProvider provider,
                                                       HttpCallbackStats* stats) {
    // Factory runs on main; mutable parser/session state is born on the IO owner.
    return [provider = std::move(provider), stats, session = std::shared_ptr<Session>{}](
               net::TcpConnection& connection, std::span<const std::byte>, bool) mutable {
        if (!session) {
            session = std::make_shared<Session>(std::move(provider), stats);
            connection.set_write_complete_callback([session](net::TcpConnection& c) {
                session->drained(c);
            });
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
} // namespace hp::app
