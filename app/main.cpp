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
    output << "Usage: hp_http_server --port <0-65535> --root <directory>\n"
           << "       hp_http_server --root <directory> --port <0-65535>\n"
           << "       hp_http_server --help\n"
           << "V0.1 / S3 minimal HTTP static file server; one GET request "
              "per connection.\n";
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
};

[[nodiscard]] Options parse_options(int argc, char* argv[]) {
    bool has_port = false;
    bool has_root = false;
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option = argv[index];
        if (option == "--port" || option == "--root") {
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

hp::net::ApplicationHandler make_handler(
    const hp::http::StaticFileService& service) {
    return [&service](std::span<const std::byte> input, bool peer_closed) {
        try {
            const std::string_view bytes(
                reinterpret_cast<const char*>(input.data()), input.size());
            const hp::http::ParseResult parsed = hp::http::parse_request(bytes);
            switch (parsed.status) {
                case hp::http::ParseStatus::need_more:
                    if (!peer_closed) {
                        return hp::net::ApplicationResult::need_more();
                    }
                    return hp::net::ApplicationResult::respond(
                        hp::http::make_error_response(
                            hp::http::Status::bad_request));
                case hp::http::ParseStatus::bad_request:
                    return hp::net::ApplicationResult::respond(
                        hp::http::make_error_response(
                            hp::http::Status::bad_request));
                case hp::http::ParseStatus::method_not_allowed:
                    return hp::net::ApplicationResult::respond(
                        hp::http::make_error_response(
                            hp::http::Status::method_not_allowed));
                case hp::http::ParseStatus::complete:
                    return hp::net::ApplicationResult::respond(
                        service.handle(parsed.request));
            }
        } catch (...) {
            return hp::net::ApplicationResult::respond(
                hp::http::make_error_response(
                    hp::http::Status::internal_server_error));
        }
        return hp::net::ApplicationResult::respond(
            hp::http::make_error_response(
                hp::http::Status::internal_server_error));
    };
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
    hp::net::TcpServer server(options.port, make_handler(service),
                              hp::http::max_request_bytes);
    const std::string port_text = std::to_string(server.bound_port());
    hp::base::info("HP HTTP Server V0.1 / S3 minimal HTTP static file server");
    hp::base::info("Listening on TCP port " + port_text + ".");
    std::cout << "V0.1 / S3 minimal HTTP static file server listening on port "
              << port_text << "." << std::endl;
    server.run();
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
