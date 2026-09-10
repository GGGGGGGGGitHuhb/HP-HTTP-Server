#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include "base/non_copyable.h"
#include "base/file_region.h"
#include <optional>
#include "net/socket.h"

namespace hp::net {
struct ReadResult {
    std::size_t bytes_read{0};
    bool would_block{false};
    bool peer_closed{false};
    int error_number{0};
};

struct WriteResult {
    std::size_t bytes_written{0};
    bool file_transfer{false};
    bool would_block{false};
    int error_number{0};
};

struct ConnectionEventResult {
    std::size_t bytes_read{0};
    std::size_t bytes_written{0};
    bool write_would_block{false};
    bool socket_error_observed{false};
    int socket_error{0};
    int socket_error_query_error{0};
    int read_error{0};
    int write_error{0};
    bool close_requested{false};
};

class ConnectionIo final : private base::NonCopyable {
public:
    static constexpr std::size_t output_limit = 9U * 1024U * 1024U;
    static constexpr std::size_t file_write_budget = 256U * 1024U;
    static constexpr std::size_t file_call_budget = 16;
    static bool output_fits(std::size_t pending, std::size_t incoming) noexcept;
    explicit ConnectionIo(Socket socket,
                          std::size_t max_input_bytes = 0) noexcept;
    ConnectionIo(ConnectionIo&&) noexcept = default;
    ConnectionIo& operator=(ConnectionIo&&) noexcept = default;

    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] ReadResult read_available();
    [[nodiscard]] WriteResult write_available();
    [[nodiscard]] ReadResult read_once();
    [[nodiscard]] int socket_error() const;
    [[nodiscard]] std::span<const std::byte> input_view() const noexcept;
    void consume(std::size_t count);
    void queue_output(std::span<const std::byte> bytes);
    void queue_file(std::span<const std::byte> header, base::FileRegion file);
    void mark_peer_half_closed() noexcept;
    [[nodiscard]] bool peer_half_closed() const noexcept;
    [[nodiscard]] bool has_pending_output() const noexcept;
    [[nodiscard]] bool accepts_input() const noexcept;
    [[nodiscard]] std::size_t pending_bytes() const noexcept;
    [[nodiscard]] bool ready_to_close() const noexcept;

private:
    friend struct ConnectionIoTestAccess;
    Socket socket_;
    std::size_t max_input_bytes_{0};
    std::vector<std::byte> input_;
    std::vector<std::byte> output_;
    std::size_t write_offset_{0};
    std::optional<base::FileRegion> file_;
    bool peer_half_closed_{false};
};

}  // namespace hp::net
