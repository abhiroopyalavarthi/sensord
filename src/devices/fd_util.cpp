#include "devices/fd_util.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0  // macOS: SO_NOSIGPIPE is set on the socket instead
#endif

namespace sensord::devices {

std::ptrdiff_t poll_read(int fd, std::span<uint8_t> buf, std::chrono::milliseconds timeout) {
  if (fd < 0) return -1;
  pollfd p{fd, POLLIN, 0};
  int r;
  do {
    r = ::poll(&p, 1, static_cast<int>(timeout.count()));
  } while (r < 0 && errno == EINTR);
  if (r < 0) return -1;
  if (r == 0) return 0;
  if (p.revents & (POLLERR | POLLNVAL)) return -1;
  if (p.revents & (POLLIN | POLLHUP)) {
    ssize_t n;
    do {
      n = ::read(fd, buf.data(), buf.size());
    } while (n < 0 && errno == EINTR);
    if (n > 0) return n;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return -1;  // EOF (peer closed / USB unplugged) or EIO
  }
  return 0;
}

bool write_all(int fd, std::span<const uint8_t> data, bool use_send,
               std::chrono::milliseconds timeout) {
  std::size_t off = 0;
  while (off < data.size()) {
    ssize_t n = use_send ? ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL)
                         : ::write(fd, data.data() + off, data.size() - off);
    if (n > 0) {
      off += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      pollfd p{fd, POLLOUT, 0};
      if (::poll(&p, 1, static_cast<int>(timeout.count())) <= 0) return false;
      if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) return false;
      continue;
    }
    return false;
  }
  return true;
}

bool set_nonblocking(int fd) {
  int flags = ::fcntl(fd, F_GETFL, 0);
  return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

}  // namespace sensord::devices
