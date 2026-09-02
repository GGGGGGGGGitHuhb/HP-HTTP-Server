#include "base/logger.h"
#include "net/tcp_server.h"

#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>

namespace {

void print_usage(std::ostream& output) {
    output << "Usage: hp_http_server --port <0-65535>\n"
           << "       hp_http_server --help\n"
           << "V0.1 / S2 is a temporary TCP echo server and does not provide "
              "HTTP service.\n";
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

int run(int argc, char* argv[]) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        print_usage(std::cout);
        return 0;
    }

    if (argc != 3 || std::string_view(argv[1]) != "--port") {
        std::cerr << "Error: expected exactly one --port <0-65535> option.\n";
        print_usage(std::cerr);
        return 2;
    }

    std::uint16_t port = 0;
    try {
        port = parse_port(argv[2]);
    } catch (const std::invalid_argument& error) {
        std::cerr << "Error: " << error.what() << ".\n";
        print_usage(std::cerr);
        return 2;
    }

    hp::net::TcpServer server(port);
    const std::string port_text = std::to_string(server.bound_port());
    hp::base::info("HP HTTP Server V0.1 / S2 TCP echo");
    hp::base::info("Listening on TCP port " + port_text + ".");
    hp::base::info("This temporary echo endpoint does not provide HTTP service.");
    std::cout << "V0.1 / S2 TCP echo listening on port " << port_text
              << "; HTTP service is not available." << std::endl;
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
