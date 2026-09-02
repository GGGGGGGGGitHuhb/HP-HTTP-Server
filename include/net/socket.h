#pragma once

#include "base/non_copyable.h"
#include <cstdint>

namespace hp::net {

class Socket final : private base::NonCopyable {
public:
    Socket() noexcept = default;
    explicit Socket(int fd) noexcept;
    ~Socket() noexcept;

    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    [[nodiscard]] static Socket create_tcp();

    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] bool valid() const noexcept;

    [[nodiscard]] int release() noexcept;
    void reset(int new_fd = -1) noexcept;

    void set_non_blocking();
    void set_reuse_address(bool enabled);
    void bind_any(std::uint16_t port);
    void listen(int backlog);
    [[nodiscard]] Socket accept_non_blocking();
    [[nodiscard]] std::uint16_t local_port() const;
    [[nodiscard]] int socket_error() const;

private:
    static constexpr int invalid_fd = -1;

    int fd_{invalid_fd};
};

}  // namespace hp::net
