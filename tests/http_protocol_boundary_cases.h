#pragma once
#include <string>
#include <vector>

namespace protocol_cases {
struct Rejection {
  std::string id, wire;
  int status;
  bool reaches_service{};
  bool permits_reset_after_complete_response{};
};

// Shared input literals and explicit protocol expectations, not a parser
// oracle.
inline std::vector<Rejection> rejections() {
  const std::string head = "GET / HTTP/1.1\r\nHost: a\r\n";
  std::string over = head + "X: ";
  over.append(16385 - over.size() - 4, 'x');
  over += "\r\n\r\n";
  return {{"G01-invalid-line", "GET  / HTTP/1.1\r\nHost: a\r\n\r\n", 400},
          {"G02-missing-host", "GET / HTTP/1.1\r\nX: a\r\n\r\n", 400},
          {"G02-header-after-close",
           head + "Connection: close\r\nBad Header: x\r\n\r\n", 400},
          {"G03-framing-before-405",
           "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\n\r\n", 400},
          {"G01-method-405", "POST / HTTP/1.1\r\nHost: a\r\n\r\n", 405},
          {"G06-service-400", "GET /bad%20target HTTP/1.1\r\nHost: a\r\n\r\n",
           400, true},
          {"G05-total-16385", std::move(over), 400, false, true}};
}

inline const std::string short_request = "GET / HTTP/1.1\r\nHost: a\r\n\r\n";

inline std::string prefix_state(std::size_t n) {
  if (short_request[n - 1] == '\r') return "pending-CR";
  if (n < 16) return "request-line";
  if (n < short_request.size() - 2) return "headers";
  return "terminal-empty-line";
}
}  // namespace protocol_cases
