#pragma once

#include <cstdio>
#include <cstdlib>
#include <source_location>

inline void requireTestCondition(bool condition,
                                 std::source_location location = std::source_location::current()) {
  if (!condition) {
    std::fprintf(stderr,
                 "Test condition failed at %s:%u\n",
                 location.file_name(),
                 static_cast<unsigned>(location.line()));
    std::abort();
  }
}
