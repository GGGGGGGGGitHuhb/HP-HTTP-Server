#pragma once
#include <chrono>

namespace hp::net {
struct ConnectionTimeouts {
  std::chrono::milliseconds idle{0};
  std::chrono::milliseconds keepAlive{0};
};
}  // namespace hp::net
