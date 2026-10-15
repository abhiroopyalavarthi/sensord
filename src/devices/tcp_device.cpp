#include "devices/tcp_device.hpp"

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "devices/fd_util.hpp"

namespace sensord::devices {

TcpDevice::TcpDevice(std::string host, uint16_t port) : host_(std::move(host)), port_(port) {}

TcpDevice::~TcpDevice() { close(); }

bool TcpDevice::open(std::string& err) {
  close();
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  const std::string port = std::to_string(port_);
  if (int rc = ::getaddrinfo(host_.c_str(), port.c_str(), &hints, &res); rc != 0) {
    err = name() + ": " + ::gai_strerror(rc);
    return false;
  }

  for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
    int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    set_nonblocking(fd);
#ifdef SO_NOSIGPIPE
    int one_np = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one_np, sizeof one_np);
#endif
    int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc != 0 && errno == EINPROGRESS) {
      pollfd p{fd, POLLOUT, 0};
      if (::poll(&p, 1, 1000) == 1) {
        int soerr = 0;
        socklen_t len = sizeof soerr;
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
        rc = soerr == 0 ? 0 : -1;
        errno = soerr;
      } else {
        errno = ETIMEDOUT;
      }
    }
    if (rc == 0) {
      int one = 1;
      ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
      fd_ = fd;
      break;
    }
    err = name() + ": " + std::strerror(errno);
    ::close(fd);
  }
  ::freeaddrinfo(res);
  return fd_ >= 0;
}

std::ptrdiff_t TcpDevice::read(std::span<uint8_t> buf, std::chrono::milliseconds timeout) {
  return poll_read(fd_, buf, timeout);
}

bool TcpDevice::write(std::span<const uint8_t> data) {
  return fd_ >= 0 && write_all(fd_, data, /*use_send=*/true);
}

void TcpDevice::close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

}  // namespace sensord::devices
