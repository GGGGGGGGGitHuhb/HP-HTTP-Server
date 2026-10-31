#pragma once
#include <chrono>
#include <optional>
#include <ostream>
#include <string>

#include "base/ResourceBudget.h"
#include "net/ConnectionTimeouts.h"

namespace hp::app {
struct ServerOptions {
  std::uint16_t port{0};
  std::string root;
  std::size_t threads{2};
  bool accessLog{false}, metricsOnExit{false};
  std::chrono::milliseconds shutdownTimeout{5000}, requestReadTimeout{10000};
  net::ConnectionTimeouts timeouts{std::chrono::milliseconds{30000},
                                   std::chrono::milliseconds{15000}};
  base::ResourceLimits resourceLimits;
  std::optional<std::uint16_t> adminPort;
};
ServerOptions parseServerOptions(int argc, char* argv[]);
void printUsage(std::ostream& output);
}  // namespace hp::app
