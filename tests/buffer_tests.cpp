#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/buffer.h"
#include "net/connection_io.h"

namespace hp::net {
struct ConnectionIoTestAccess {
  static auto input_capacity(const ConnectionIo& io) {
    return io.input_.capacity();
  }

  static auto output_capacity(const ConnectionIo& io) {
    return io.output_.capacity();
  }

  static auto output(const ConnectionIo& io) {
    return io.output_.readable_view();
  }
};
}  // namespace hp::net

namespace {
using hp::base::Buffer;
using hp::net::ConnectionIo;
using hp::net::ConnectionIoTestAccess;
using hp::net::Socket;
bool fail_allocation = false, count_allocation = false;
std::size_t allocations = 0, allocation_hits = 0, allocated_bytes = 0;
const char* incoming = "abcdefgh";
std::size_t incoming_size = 8, recv_calls = 0;
void* recv_target = nullptr;
int recv_error = 0, send_phase = -1;
int watched_file = -1, file_closes = 0;

void require(bool value, const char* text) {
  if (!value) throw std::runtime_error(text);
}

std::span<const std::byte> bytes(std::string_view text) {
  return std::as_bytes(std::span(text.data(), text.size()));
}

std::string text(const Buffer& buffer) {
  const auto view = buffer.readable_view();
  return {reinterpret_cast<const char*>(view.data()), view.size()};
}

template <class E, class F>
void rejects(F operation, const char* message) {
  bool rejected = false;
  try {
    operation();
  } catch (const E&) {
    rejected = true;
  }
  require(rejected, message);
}

void buffer_contract() {
  Buffer b(8);
  require(b.readable_bytes() == 0 && b.capacity() == 0, "lazy empty buffer");
  b.append(bytes(std::string_view("ab\0cdefg", 8)));
  const auto* suffix = b.readable_view().data() + 3;
  b.consume(3);
  require(b.readable_view().data() == suffix && text(b) == "cdefg",
          "consume advances without moving suffix");
  rejects<std::out_of_range>([&] { b.consume(6); }, "consume bounds");
  rejects<std::length_error>([&] { b.prepare(SIZE_MAX); },
                             "SIZE_MAX overflow rejection");
  rejects<std::out_of_range>([&] { b.commit(1); },
                             "unprepared commit rejection");
  require(text(b) == "cdefg", "rejection preserves readable bytes");
  b.append(bytes("hij"));
  require(text(b) == "cdefghij" && b.capacity() == 8,
          "necessary compaction exact bytes");
  rejects<std::length_error>([&] { b.append(bytes("x")); }, "limit plus one");
  b.consume(8);
  require(b.readable_bytes() == 0 && b.capacity() == 8,
          "full consume resets cursors");
  auto tail = b.prepare(7);
  std::memcpy(tail.data(), "1234567", 7);
  rejects<std::out_of_range>([&] { b.commit(8); },
                             "commit exceeds reservation");
  b.commit(7);
  require(text(b) == "1234567", "limit minus one");
  b.append(bytes("8"));
  require(text(b) == "12345678", "limit exact");
  Buffer moved(std::move(b));
  require(
      text(moved) == "12345678" && b.readable_bytes() == 0 && b.capacity() == 0,
      "move constructor valid source");
  b.append(bytes("reuse"));
  b = std::move(moved);
  require(text(b) == "12345678" && moved.readable_bytes() == 0,
          "move assignment valid source");
  auto& alias = b;
  b = std::move(alias);
  require(text(b) == "12345678", "self move");
  Buffer growing(32);
  growing.append(bytes("abcd"));
  growing.consume(1);
  fail_allocation = true;
  rejects<std::bad_alloc>([&] { growing.append(bytes("12345")); },
                          "growth allocation failure");
  require(text(growing) == "bcd" && growing.capacity() == 4,
          "failed growth strong guarantee");
  growing.append(bytes("12345"));
  require(text(growing) == "bcd12345" && growing.capacity() == 8,
          "growth preserves suffix");
  std::cout << "Buffer consume_move=0 NUL bounds overflow prepare commit move "
               "failure PASS\n";
}

void direct_receive() {
  ConnectionIo io{Socket{}, 16384};
  recv_error = EINTR;
  const auto read = io.read_once();
  require(read.bytes_read == 8 && io.input_view().data() == recv_target,
          "recv targets owned committed storage without second copy");
  io.consume(4);
  const auto* remaining = io.input_view().data();
  recv_error = EAGAIN;
  require(io.read_once().would_block && io.input_view().data() == remaining &&
              io.input_view().size() == 4,
          "EAGAIN preserves suffix");
  recv_error = ECONNRESET;
  require(
      io.read_once().error_number == ECONNRESET && io.input_view().size() == 4,
      "read error preserves suffix");
  incoming_size = 0;
  require(io.read_once().peer_closed && io.input_view().size() == 4,
          "EOF preserves suffix");

  std::string block(4096, 'x');
  incoming = block.data();
  incoming_size = block.size();
  ConnectionIo full{Socket{}, 16384};
  require(full.read_once().bytes_read == 4096, "initial lazy 4KiB recv");
  const auto old_calls = recv_calls;
  fail_allocation = true;
  rejects<std::bad_alloc>([&] { (void)full.read_once(); },
                          "receive prepare allocation failure");
  require(recv_calls == old_calls && full.input_view().size() == 4096,
          "prepare failure precedes recv and preserves bytes");
  while (full.input_view().size() < 16384) (void)full.read_once();
  require(full.read_once().error_number == EMSGSIZE &&
              ConnectionIoTestAccess::input_capacity(full) <= 16384,
          "production input actual capacity limit");
  ConnectionIo unlimited{Socket{}};
  for (int i = 0; i < 5; ++i) (void)unlimited.read_once();
  require(unlimited.input_view().size() == 20480,
          "zero max input retains unlimited library semantics");
  std::cout << "recv owned_address=1 second_copy=0 error/EINTR/EOF "
               "allocation_before_recv=1 input_capacity="
            << ConnectionIoTestAccess::input_capacity(full)
            << " unlimited=20480 PASS\n";
}

hp::base::FileRegion empty_file() {
  return {hp::base::UniqueFd(::open("/dev/null", O_RDONLY)), 0, 0};
}

void output_storage() {
  ConnectionIo io{Socket{}};
  std::vector<std::byte> warm(64);
  io.queue_output(warm);
  (void)io.write_available();
  io.queue_output(std::span(warm).first(32));
  send_phase = 0;
  (void)io.write_available();
  auto suffix = ConnectionIoTestAccess::output(io);
  io.queue_output(std::span(warm).first(1));
  require(ConnectionIoTestAccess::output(io).data() == suffix.data() &&
              io.pending_bytes() == 29,
          "append with tail room preserves suffix address");
  send_phase = -1;
  (void)io.write_available();
  for (std::size_t size : {65536U, 65537U}) {
    ConnectionIo exact{Socket{}};
    std::vector<std::byte> payload(size);
    exact.queue_output(payload);
    require(ConnectionIoTestAccess::output_capacity(exact) == size,
            "explicit output allocation capacity");
    (void)exact.write_available();
    require(ConnectionIoTestAccess::output_capacity(exact) ==
                (size == 65536 ? size : 0),
            "idle output exact64KiB retains larger releases");
  }
  std::vector<std::byte> big(65537);
  for (int i = 0; i < 100; ++i) {
    io.queue_output(big);
    (void)io.write_available();
    require(ConnectionIoTestAccess::output_capacity(io) == 0,
            "100 large drains release capacity");
  }
  io.queue_file(std::span(warm).first(16), empty_file());
  (void)io.write_available();
  allocations = 0;
  allocated_bytes = 0;
  for (int i = 0; i < 1000; ++i) {
    auto file = empty_file();
    count_allocation = true;
    io.queue_file(std::span(warm).first(16), std::move(file));
    count_allocation = false;
    (void)io.write_available();
  }
  require(allocations == 0, "1000 warm file headers reuse allocation");
  io.queue_output(std::span(warm).first(8));
  fail_allocation = true;
  rejects<std::bad_alloc>([&] { io.queue_output(big); },
                          "output growth allocation failure");
  require(io.pending_bytes() == 8, "output growth failure preserves pending");
  ConnectionIo header{Socket{}};
  auto file = empty_file();
  watched_file = file.fd();
  file_closes = 0;
  fail_allocation = true;
  rejects<std::bad_alloc>([&] { header.queue_file(warm, std::move(file)); },
                          "header allocation failure");
  require(file_closes == 1 && header.pending_bytes() == 0,
          "file header failure closes exactly once no partial publish");
  watched_file = -1;
  auto held = hp::base::FileRegion(
      hp::base::UniqueFd(::open("/dev/null", O_RDONLY)), 0, 100);
  header.queue_file(bytes("HDR"), std::move(held));
  require(header.pending_bytes() == 103,
          "logical pending includes file remaining");
  require(allocation_hits == 4, "four allocation failures precisely consumed");
  Buffer peak(ConnectionIo::output_limit);
  std::vector<std::byte> first(5U * 1024U * 1024U), second(4U * 1024U * 1024U);
  peak.append(first);
  const auto old_capacity = peak.capacity();
  allocated_bytes = 0;
  allocations = 0;
  count_allocation = true;
  peak.append(second);
  count_allocation = false;
  require(allocations == 1 && allocated_bytes == ConnectionIo::output_limit &&
              peak.capacity() == ConnectionIo::output_limit &&
              old_capacity + allocated_bytes <= 2 * ConnectionIo::output_limit,
          "output allocation trace bounds final and transient old plus new");
  std::cout << "output growth old=" << old_capacity
            << " new=" << allocated_bytes
            << " transient=" << old_capacity + allocated_bytes << "\n";
  std::cout << "output tail_move=0 header_allocations_1000=0 retention64/64+1 "
               "large100=0 allocation_hits=4 file_closes=1 PASS\n";
}
}  // namespace

extern "C" void* __real__Znwm(std::size_t);

extern "C" void* __wrap__Znwm(std::size_t n) {
  if (fail_allocation) {
    fail_allocation = false;
    ++allocation_hits;
    throw std::bad_alloc();
  }
  if (count_allocation) {
    ++allocations;
    allocated_bytes += n;
  }
  return __real__Znwm(n);
}

extern "C" int __real_close(int);

extern "C" int __wrap_close(int fd) {
  if (fd == watched_file) ++file_closes;
  return __real_close(fd);
}

extern "C" ssize_t __wrap_recv(int, void* data, std::size_t n, int) {
  ++recv_calls;
  recv_target = data;
  if (recv_error) {
    errno = recv_error;
    recv_error = 0;
    return -1;
  }
  n = std::min(n, incoming_size);
  if (n) std::memcpy(data, incoming, n);
  return n;
}

extern "C" ssize_t __wrap_send(int, const void*, std::size_t n, int) {
  if (send_phase == 0) {
    send_phase = 1;
    return std::min(n, std::size_t{4});
  }
  if (send_phase == 1) {
    errno = EAGAIN;
    return -1;
  }
  return n;
}

int main(int argc, char** argv) {
  try {
    const std::string mode = argc == 2 ? argv[1] : "all";
    if (mode == "all" || mode == "buffer") buffer_contract();
    if (mode == "all" || mode == "recv") direct_receive();
    if (mode == "all" || mode == "output") output_storage();
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "buffer assertion: " << e.what() << '\n';
    return 1;
  }
}
