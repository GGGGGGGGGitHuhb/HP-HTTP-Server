#include "base/logger.h"

#include <exception>
#include <iostream>
#include <string_view>

namespace {

void print_usage(std::ostream& output) {
    output << "Usage: hp_http_server [--help]\n"
           << "V0.1 / S1 is a project skeleton and does not provide HTTP service.\n";
}

int run(int argc, char* argv[]) {
    if (argc == 1) {
        hp::base::info("HP HTTP Server V0.1 / S1 skeleton");
        hp::base::info("This stage does not provide HTTP service.");
        return 0;
    }

    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        print_usage(std::cout);
        return 0;
    }

    std::cerr << "Error: unknown option";
    if (argc >= 2) {
        std::cerr << " '" << argv[1] << "'";
    }
    std::cerr << ".\n";
    print_usage(std::cerr);
    return 2;
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
