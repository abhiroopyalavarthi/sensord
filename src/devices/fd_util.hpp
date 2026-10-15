#pragma once
// Small POSIX helpers shared by the serial and TCP links.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sensord::devices {

// poll() for readability, then read(). Returns >0 bytes, 0 on timeout,
// -1 on hangup/EOF/error.
std::ptrdiff_t poll_read(int fd, std::span<uint8_t> buf, std::chrono::milliseconds timeout);

// Writes every byte, waiting on POLLOUT if the fd is non-blocking.
// `use_send` uses send(MSG_NOSIGNAL) so a dead socket can't raise SIGPIPE.
bool write_all(int fd, std::span<const uint8_t> data, bool use_send,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(500));

bool set_nonblocking(int fd);

}  // namespace sensord::devices
