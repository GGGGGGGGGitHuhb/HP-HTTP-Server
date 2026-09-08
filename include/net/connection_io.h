#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>
#include "base/non_copyable.h"
#include "net/socket.h"

namespace hp::net {
enum class ApplicationStatus {
    need_more,
    response,
};

struct ApplicationResult {
    ApplicationStatus status{ApplicationStatus::need_more};
    std::vector<std::byte> response;

    [[nodiscard]] static ApplicationResult need_more();
    [[nodiscard]] static ApplicationResult respond(
        std::vector<std::byte> bytes);
};

// The input span is borrowed only for this call. Responses are copied into
// connection-owned output storage before the application result is destroyed.
using ApplicationHandler = std::function<ApplicationResult(
    std::span<const std::byte> input, bool peer_closed)>;

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
    explicit ConnectionIo(Socket socket, ApplicationHandler handler = {},
                          std::size_t max_input_bytes = 0) noexcept;
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
    [[nodiscard]] bool accepts_input() const noexcept;
    [[nodiscard]] std::size_t pending_bytes() const noexcept;
    [[nodiscard]] bool ready_to_close() const noexcept;

   private:
    [[nodiscard]] bool process_application(bool peer_closed);

    Socket socket_;
    ApplicationHandler application_handler_;
    std::size_t max_input_bytes_{0};
    std::vector<std::byte> input_;
    std::vector<std::byte> output_;
    std::size_t write_offset_{0};
    bool peer_half_closed_{false};
    bool response_queued_{false};
    bool close_after_write_{false};
};

}  // namespace hp::net
