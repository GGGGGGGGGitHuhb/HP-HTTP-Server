#include "http/HttpResponse.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace hp::http {
namespace {

char lowerCharacter(unsigned char character) {
  return static_cast<char>(std::tolower(character));
}

std::string_view reasonPhrase(Status status) {
  switch (status) {
    case Status::kOk:
      return "OK";
    case Status::kBadRequest:
      return "Bad Request";
    case Status::kForbidden:
      return "Forbidden";
    case Status::kNotFound:
      return "Not Found";
    case Status::kMethodNotAllowed:
      return "Method Not Allowed";
    case Status::kInternalServerError:
      return "Internal Server Error";
  }
  throw std::invalid_argument("unsupported HTTP status");
}

std::string_view errorBody(Status status) {
  switch (status) {
    case Status::kBadRequest:
      return "400 Bad Request\n";
    case Status::kForbidden:
      return "403 Forbidden\n";
    case Status::kNotFound:
      return "404 Not Found\n";
    case Status::kMethodNotAllowed:
      return "405 Method Not Allowed\n";
    case Status::kInternalServerError:
      return "500 Internal Server Error\n";
    case Status::kOk:
      break;
  }
  throw std::invalid_argument("200 is not an error response");
}

std::span<const std::byte> asBytes(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

}  // namespace

std::vector<std::byte> makeResponseHeader(Status status,
                                          std::size_t contentLength,
                                          std::string_view contentType,
                                          bool includeAllowGet,
                                          ConnectionPolicy policy) {
  std::string header = "HTTP/1.1 " + std::to_string(static_cast<int>(status)) +
                       " " + std::string(reasonPhrase(status)) + "\r\n";
  header += "Content-Length: " + std::to_string(contentLength) + "\r\n";
  header += "Content-Type: " + std::string(contentType) + "\r\n";
  header += policy == ConnectionPolicy::kClose ? "Connection: close\r\n"
                                               : "Connection: keep-alive\r\n";
  if (includeAllowGet) {
    header += "Allow: GET\r\n";
  }
  header += "\r\n";

  const auto bytes = asBytes(header);
  return {bytes.begin(), bytes.end()};
}

std::vector<std::byte> makeResponse(Status status,
                                    std::span<const std::byte> body,
                                    std::string_view contentType,
                                    bool includeAllowGet,
                                    ConnectionPolicy policy) {
  auto response = makeResponseHeader(status,
                                     body.size(),
                                     contentType,
                                     includeAllowGet,
                                     policy);
  response.reserve(response.size() + body.size());
  response.insert(response.end(), body.begin(), body.end());
  return response;
}

std::vector<std::byte> makeErrorResponse(Status status,
                                         ConnectionPolicy policy) {
  const std::string_view body = errorBody(status);
  return makeResponse(status,
                      asBytes(body),
                      "text/plain; charset=utf-8",
                      status == Status::kMethodNotAllowed,
                      policy);
}

std::string contentTypeForPath(std::string_view path) {
  const std::size_t slash = path.find_last_of('/');
  const std::size_t dot = path.find_last_of('.');
  if (dot == std::string_view::npos ||
      (slash != std::string_view::npos && dot < slash)) {
    return "application/octet-stream";
  }
  std::string extension(path.substr(dot));
  std::transform(extension.begin(),
                 extension.end(),
                 extension.begin(),
                 lowerCharacter);
  if (extension == ".html" || extension == ".htm") {
    return "text/html; charset=utf-8";
  }
  if (extension == ".css") return "text/css; charset=utf-8";
  if (extension == ".js") return "application/javascript";
  if (extension == ".txt") return "text/plain; charset=utf-8";
  if (extension == ".json") return "application/json";
  if (extension == ".png") return "image/png";
  if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
  if (extension == ".gif") return "image/gif";
  if (extension == ".svg") return "image/svg+xml";
  return "application/octet-stream";
}

}  // namespace hp::http
