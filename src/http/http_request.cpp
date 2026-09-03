#include "http/http_request.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace hp::http {
namespace {

constexpr std::string_view header_end = "\r\n\r\n";

bool is_token_character(unsigned char character) {
    if (std::isalnum(character) != 0) {
        return true;
    }
    constexpr std::string_view punctuation = "!#$%&'*+-.^_`|~";
    return punctuation.find(static_cast<char>(character)) !=
           std::string_view::npos;
}

bool is_valid_token(std::string_view token) {
    return !token.empty() &&
           std::all_of(token.begin(), token.end(), [](char character) {
               return is_token_character(static_cast<unsigned char>(character));
           });
}

bool has_invalid_line_endings(std::string_view bytes) {
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (bytes[index] == '\n' && (index == 0 || bytes[index - 1] != '\r')) {
            return true;
        }
        if (bytes[index] == '\r' && index + 1 < bytes.size() &&
            bytes[index + 1] != '\n') {
            return true;
        }
    }
    return false;
}

std::string_view trim_optional_whitespace(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

bool case_insensitive_equal(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

bool valid_target(std::string_view target) {
    if (target.empty() || target.front() != '/' ||
        target.find('#') != std::string_view::npos) {
        return false;
    }
    for (char character : target) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte == 0 || byte <= 0x20U || byte == 0x7fU) {
            return false;
        }
    }
    return true;
}

ParseResult bad_request() { return {ParseStatus::bad_request, {}, 0}; }

}  // namespace

ParseResult parse_request(std::string_view bytes) {
    const std::size_t complete_end = bytes.find(header_end);
    std::size_t examined_size = bytes.size();
    std::size_t consumed = 0;
    if (complete_end != std::string_view::npos) {
        consumed = complete_end + header_end.size();
        if (consumed > max_request_bytes) {
            return bad_request();
        }
        examined_size = consumed;
    } else if (bytes.size() >= max_request_bytes) {
        return bad_request();
    }

    const std::string_view examined = bytes.substr(0, examined_size);
    if (examined.find('\0') != std::string_view::npos ||
        has_invalid_line_endings(examined)) {
        return bad_request();
    }

    const std::size_t request_line_end = examined.find("\r\n");
    if (request_line_end == std::string_view::npos) {
        if (bytes.size() >= max_request_line_bytes) {
            return bad_request();
        }
        return {};
    }
    if (request_line_end > max_request_line_bytes) {
        return bad_request();
    }
    if (complete_end == std::string_view::npos) {
        return {};
    }

    const std::string_view request_line = examined.substr(0, request_line_end);
    const std::size_t first_space = request_line.find(' ');
    if (first_space == std::string_view::npos) {
        return bad_request();
    }
    const std::size_t second_space = request_line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos ||
        request_line.find(' ', second_space + 1) != std::string_view::npos) {
        return bad_request();
    }

    const std::string_view method = request_line.substr(0, first_space);
    const std::string_view target =
        request_line.substr(first_space + 1, second_space - first_space - 1);
    const std::string_view version = request_line.substr(second_space + 1);
    if (!is_valid_token(method) || !valid_target(target) ||
        version != "HTTP/1.1") {
        return bad_request();
    }

    std::size_t host_count = 0;
    std::size_t line_start = request_line_end + 2;
    while (line_start < complete_end) {
        const std::size_t line_end = bytes.find("\r\n", line_start);
        if (line_end == std::string_view::npos || line_end > complete_end) {
            return bad_request();
        }
        const std::string_view line =
            bytes.substr(line_start, line_end - line_start);
        if (line.empty() || line.front() == ' ' || line.front() == '\t') {
            return bad_request();
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos ||
            !is_valid_token(line.substr(0, colon))) {
            return bad_request();
        }
        const std::string_view value = line.substr(colon + 1);
        for (char character : value) {
            const auto byte = static_cast<unsigned char>(character);
            if (byte == 0 || byte == 0x7fU ||
                (byte < 0x20U && character != '\t')) {
                return bad_request();
            }
        }
        if (case_insensitive_equal(line.substr(0, colon), "Host")) {
            ++host_count;
            if (trim_optional_whitespace(value).empty()) {
                return bad_request();
            }
        }
        line_start = line_end + 2;
    }
    if (host_count != 1) {
        return bad_request();
    }

    HttpRequest request{std::string(method), std::string(target)};
    const ParseStatus status = method == "GET"
                                   ? ParseStatus::complete
                                   : ParseStatus::method_not_allowed;
    return {status, std::move(request), consumed};
}

}  // namespace hp::http
