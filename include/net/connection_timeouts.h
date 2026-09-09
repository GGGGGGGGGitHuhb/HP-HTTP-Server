#pragma once
#include <chrono>

namespace hp::net {
struct ConnectionTimeouts {
    std::chrono::milliseconds idle{0};
    std::chrono::milliseconds keep_alive{0};
};
}
