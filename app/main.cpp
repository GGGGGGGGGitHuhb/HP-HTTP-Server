#include "http_connection_handler.h"
#include "signal_watcher.h"
#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "base/logger.h"
#include "http/http_request.h"
#include "http/http_response.h"
#include "http/static_file_service.h"
#include "net/tcp_server.h"

namespace {

void print_usage(std::ostream& output) {
    output
        << "Usage: hp_http_server --port <0-65535> --root <directory> [--threads <0-64>]\n"
        << "       hp_http_server --root <directory> --port <0-65535>\n"
        << "       hp_http_server --help\n"
        << "V0.1 / S3 minimal HTTP static file server; restricted GET/keep-alive.\n"
        << "--threads defaults to 2 workers; 0 selects a single Reactor.\n"
        << "--idle-timeout-ms <0-86400000> defaults to 30000; "
           "--keep-alive-timeout-ms <0-86400000> defaults to 15000.\n"
        << "--shutdown-timeout-ms <0-60000> defaults to 5000; 0 closes immediately.\n"
        << "SIGINT/SIGTERM drain current output; another observed signal forces close.\n"
        << "Idle/keep-alive: 0 disables; expiry closes silently and may truncate a response.\n";
}

[[nodiscard]] std::uint16_t parse_port(std::string_view text) {
    unsigned int value = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value, 10);
    if (text.empty() || error != std::errc{} ||
        end != text.data() + text.size() || value > 65535U) {
        throw std::invalid_argument("port must be a decimal value in 0-65535");
    }
    return static_cast<std::uint16_t>(value);
}

struct Options {
    std::uint16_t port{0};
    std::string root;
    std::size_t threads{2};
    std::chrono::milliseconds shutdown_timeout{5000};
    hp::net::ConnectionTimeouts timeouts{std::chrono::milliseconds(30000),
                                         std::chrono::milliseconds(15000)};
};

[[nodiscard]] Options parse_options(int argc, char* argv[]) {
    bool has_port = false;
    bool has_root = false;
    bool has_threads = false;
    bool has_idle = false, has_keep = false, has_shutdown = false;
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option = argv[index];
        if (option == "--port" || option == "--root" || option == "--threads" ||
            option == "--idle-timeout-ms" ||
            option == "--keep-alive-timeout-ms" ||
            option == "--shutdown-timeout-ms") {
            if (index + 1 >= argc) {
                throw std::invalid_argument("option value is missing");
            }
            const std::string_view value = argv[++index];
            if (option == "--port") {
                if (has_port) {
                    throw std::invalid_argument(
                        "--port appears more than once");
                }
                options.port = parse_port(value);
                has_port = true;
            } else if (option == "--threads") {
                if (has_threads)
                    throw std::invalid_argument(
                        "--threads appears more than once");
                unsigned int count = 0;
                const auto [end, error] = std::from_chars(
                    value.data(), value.data() + value.size(), count, 10);
                if (value.empty() || error != std::errc{} ||
                    end != value.data() + value.size() || count > 64)
                    throw std::invalid_argument(
                        "threads must be decimal in 0-64");
                options.threads = count;
                has_threads = true;
            } else if (option == "--shutdown-timeout-ms") {
                if (has_shutdown)
                    throw std::invalid_argument(
                        "shutdown timeout appears more than once");
                unsigned int milliseconds = 0;
                const auto [end, error] =
                    std::from_chars(value.data(), value.data() + value.size(),
                                    milliseconds, 10);
                if (value.empty() || error != std::errc{} ||
                    end != value.data() + value.size() || milliseconds > 60000)
                    throw std::invalid_argument(
                        "shutdown timeout must be decimal in 0-60000ms");
                options.shutdown_timeout =
                    std::chrono::milliseconds(milliseconds);
                has_shutdown = true;
            } else if (option == "--idle-timeout-ms" ||
                       option == "--keep-alive-timeout-ms") {
                bool& seen =
                    option == "--idle-timeout-ms" ? has_idle : has_keep;
                if (seen)
                    throw std::invalid_argument(
                        "timeout appears more than once");
                unsigned int value_ms = 0;
                const auto [end, error] = std::from_chars(
                    value.data(), value.data() + value.size(), value_ms, 10);
                if (value.empty() || error != std::errc{} ||
                    end != value.data() + value.size() || value_ms > 86400000)
                    throw std::invalid_argument(
                        "timeout must be decimal in 0-86400000ms");
                auto& duration = option == "--idle-timeout-ms"
                                     ? options.timeouts.idle
                                     : options.timeouts.keep_alive;
                duration = std::chrono::milliseconds(value_ms);
                seen = true;
            } else {
                if (has_root) {
                    throw std::invalid_argument(
                        "--root appears more than once");
                }
                if (value.empty()) {
                    throw std::invalid_argument("root directory is empty");
                }
                options.root = value;
                has_root = true;
            }
            continue;
        }
        throw std::invalid_argument("unknown option");
    }
    if (!has_port || !has_root) {
        throw std::invalid_argument(
            "exactly one --port and one --root are required");
    }
    return options;
}

int run(int argc, char* argv[]) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        print_usage(std::cout);
        return 0;
    }

    Options options;
    try {
        options = parse_options(argc, argv);
    } catch (const std::invalid_argument& error) {
        std::cerr << "Error: " << error.what() << ".\n";
        print_usage(std::cerr);
        return 2;
    }

    hp::http::StaticFileService service(options.root);
    hp::app::SignalWatcher signals;
    hp::net::TcpServer server(options.port, hp::app::make_http_factory(service),
                              hp::http::max_request_bytes, options.threads,
                              options.timeouts);
    bool draining = false;
    server.watch_control_fd(signals.fd(), [&](std::uint32_t) {
        while (const int signal = signals.next()) {
            if (draining) {
                server.force_shutdown();
            } else {
                draining = true;
                server.request_graceful_shutdown(
                    hp::timer::TimerQueue::Clock::now() +
                    options.shutdown_timeout);
            }
            hp::base::info(
                "Shutdown signal observed: " + std::to_string(signal) + ".");
        }
    });
    const std::string port_text = std::to_string(server.bound_port());
    hp::base::info("HP HTTP Server V0.1 / S3 minimal HTTP static file server");
    hp::base::info("Listening on TCP port " + port_text + ".");
    std::cout << "V0.1 / S3 minimal HTTP static file server listening on port "
              << port_text << "." << std::endl;
    server.run();
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        hp::base::error(error.what());
    } catch (...) {
        hp::base::error("Unknown fatal error.");
    }
    return 1;
}
