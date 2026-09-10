#include "http/http_response.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace hp::http {
namespace {

std::string_view reason_phrase(Status status) {
    switch (status) {
        case Status::ok:
            return "OK";
        case Status::bad_request:
            return "Bad Request";
        case Status::forbidden:
            return "Forbidden";
        case Status::not_found:
            return "Not Found";
        case Status::method_not_allowed:
            return "Method Not Allowed";
        case Status::internal_server_error:
            return "Internal Server Error";
    }
    throw std::invalid_argument("unsupported HTTP status");
}

std::string_view error_body(Status status) {
    switch (status) {
        case Status::bad_request:
            return "400 Bad Request\n";
        case Status::forbidden:
            return "403 Forbidden\n";
        case Status::not_found:
            return "404 Not Found\n";
        case Status::method_not_allowed:
            return "405 Method Not Allowed\n";
        case Status::internal_server_error:
            return "500 Internal Server Error\n";
        case Status::ok:
            break;
    }
    throw std::invalid_argument("200 is not an error response");
}

std::span<const std::byte> as_bytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

}  // namespace

std::vector<std::byte> make_response_header(Status status,
                                            std::size_t content_length,
                                            std::string_view content_type,
                                            bool include_allow_get,
                                            ConnectionPolicy policy) {
    std::string header = "HTTP/1.1 " +
                         std::to_string(static_cast<int>(status)) + " " +
                         std::string(reason_phrase(status)) + "\r\n";
    header += "Content-Length: " + std::to_string(content_length) + "\r\n";
    header += "Content-Type: " + std::string(content_type) + "\r\n";
    header += policy == ConnectionPolicy::close ? "Connection: close\r\n"
                                                : "Connection: keep-alive\r\n";
    if (include_allow_get) {
        header += "Allow: GET\r\n";
    }
    header += "\r\n";

    const auto bytes = as_bytes(header);
    return {bytes.begin(), bytes.end()};
}

std::vector<std::byte> make_response(Status status,
                                     std::span<const std::byte> body,
                                     std::string_view content_type,
                                     bool include_allow_get,
                                     ConnectionPolicy policy) {
    auto response = make_response_header(status, body.size(), content_type,
                                         include_allow_get, policy);
    response.reserve(response.size() + body.size());
    response.insert(response.end(), body.begin(), body.end());
    return response;
}

std::vector<std::byte> make_error_response(Status status,
                                           ConnectionPolicy policy) {
    const std::string_view body = error_body(status);
    return make_response(status, as_bytes(body), "text/plain; charset=utf-8",
                         status == Status::method_not_allowed, policy);
}

std::string content_type_for_path(std::string_view path) {
    const std::size_t slash = path.find_last_of('/');
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos ||
        (slash != std::string_view::npos && dot < slash)) {
        return "application/octet-stream";
    }
    std::string extension(path.substr(dot));
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    if (extension == ".html" || extension == ".htm") {
        return "text/html; charset=utf-8";
    }
    if (extension == ".css")
        return "text/css; charset=utf-8";
    if (extension == ".js")
        return "application/javascript";
    if (extension == ".txt")
        return "text/plain; charset=utf-8";
    if (extension == ".json")
        return "application/json";
    if (extension == ".png")
        return "image/png";
    if (extension == ".jpg" || extension == ".jpeg")
        return "image/jpeg";
    if (extension == ".gif")
        return "image/gif";
    if (extension == ".svg")
        return "image/svg+xml";
    return "application/octet-stream";
}

}  // namespace hp::http
