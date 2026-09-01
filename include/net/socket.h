#pragma once

#include "base/non_copyable.h"

namespace hp::net {

class Socket final : private base::NonCopyable {
public:
    Socket() noexcept = default;
    explicit Socket(int fd) noexcept;
    ~Socket() noexcept;

    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] bool valid() const noexcept;

    [[nodiscard]] int release() noexcept;
    void reset(int new_fd = -1) noexcept;

    void set_non_blocking();
    void set_reuse_address(bool enabled);

private:
    static constexpr int invalid_fd = -1;

    int fd_{invalid_fd};
};

}  // namespace hp::net
