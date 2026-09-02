#pragma once

#include "base/non_copyable.h"
#include "net/epoller.h"
#include "net/socket.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace hp::net {

struct ReadResult {
    std::size_t bytes_read{0};
    bool would_block{false};
    bool peer_closed{false};
    int error_number{0};
};

struct WriteResult {
    std::size_t bytes_written{0};
    bool would_block{false};
    int error_number{0};
};

struct ConnectionEventResult {
    std::size_t bytes_read{0};
    std::size_t bytes_written{0};
    bool socket_error_observed{false};
    int socket_error{0};
    int socket_error_query_error{0};
    int read_error{0};
    int write_error{0};
    bool close_requested{false};
};

class ConnectionIo final : private base::NonCopyable {
public:
    explicit ConnectionIo(Socket socket) noexcept;
    ConnectionIo(ConnectionIo&&) noexcept = default;
    ConnectionIo& operator=(ConnectionIo&&) noexcept = default;

    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] ReadResult read_available();
    [[nodiscard]] WriteResult write_available();
    [[nodiscard]] ConnectionEventResult handle_event(std::uint32_t events);
    void queue_output(std::span<const std::byte> bytes);
    void mark_peer_half_closed() noexcept;
    [[nodiscard]] bool peer_half_closed() const noexcept;
    [[nodiscard]] bool has_pending_output() const noexcept;
    [[nodiscard]] std::size_t pending_bytes() const noexcept;
    [[nodiscard]] bool ready_to_close() const noexcept;

private:
    Socket socket_;
    std::vector<std::byte> output_;
    std::size_t write_offset_{0};
    bool peer_half_closed_{false};
};

class TcpServer final : private base::NonCopyable {
public:
    explicit TcpServer(std::uint16_t requested_port);

    [[nodiscard]] std::uint16_t bound_port() const noexcept;
    [[noreturn]] void run();

private:
    struct ConnectionState {
        ConnectionIo io;
        std::uint32_t generation;
    };

    static constexpr std::uint64_t listener_token =
        static_cast<std::uint64_t>(-1);
    static constexpr std::uint32_t listener_events = EPOLLIN;
    static constexpr std::uint32_t connection_read_events =
        EPOLLIN | EPOLLRDHUP;

    [[nodiscard]] static Socket create_listener(std::uint16_t port);
    [[nodiscard]] static std::uint64_t make_token(int fd,
                                                   std::uint32_t generation);
    [[nodiscard]] static int token_fd(std::uint64_t token);
    [[nodiscard]] static std::uint32_t token_generation(std::uint64_t token);

    void handle_listener_event(std::uint32_t events);
    void accept_ready_connections();
    void handle_connection_event(std::uint64_t token, std::uint32_t events);
    void update_interest(ConnectionState& connection);
    void close_connection(int fd) noexcept;
    [[nodiscard]] std::uint32_t next_generation() noexcept;

    Socket listener_;
    Epoller epoller_;
    std::unordered_map<int, ConnectionState> connections_;
    std::uint16_t bound_port_{0};
    std::uint32_t next_generation_{1};
};

}  // namespace hp::net
