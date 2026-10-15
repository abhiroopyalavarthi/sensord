#include "devices/serial_device.hpp"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "devices/fd_util.hpp"

namespace sensord::devices {

namespace {
speed_t to_speed(int baud) {
  switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
    default: return B115200;
  }
}
}  // namespace

SerialDevice::SerialDevice(std::string path, int baud) : path_(std::move(path)), baud_(baud) {}

SerialDevice::~SerialDevice() { close(); }

bool SerialDevice::open(std::string& err) {
  close();
  fd_ = ::open(path_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd_ < 0) {
    err = path_ + ": " + std::strerror(errno);
    return false;
  }

  termios tio{};
  if (::tcgetattr(fd_, &tio) != 0) {
    err = path_ + ": tcgetattr: " + std::strerror(errno);
    close();
    return false;
  }
  ::cfmakeraw(&tio);              // no echo, no line editing, 8-bit clean
  tio.c_cflag |= CLOCAL | CREAD;  // ignore modem lines, enable receiver
  tio.c_cflag &= ~static_cast<tcflag_t>(CSTOPB);
#ifdef CRTSCTS
  tio.c_cflag &= ~static_cast<tcflag_t>(CRTSCTS);
#endif
  tio.c_cc[VMIN] = 0;
  tio.c_cc[VTIME] = 0;
  ::cfsetispeed(&tio, to_speed(baud_));
  ::cfsetospeed(&tio, to_speed(baud_));
  if (::tcsetattr(fd_, TCSANOW, &tio) != 0) {
    err = path_ + ": tcsetattr: " + std::strerror(errno);
    close();
    return false;
  }
  ::tcflush(fd_, TCIOFLUSH);
  return true;
}

std::ptrdiff_t SerialDevice::read(std::span<uint8_t> buf, std::chrono::milliseconds timeout) {
  return poll_read(fd_, buf, timeout);
}

bool SerialDevice::write(std::span<const uint8_t> data) {
  return fd_ >= 0 && write_all(fd_, data, /*use_send=*/false);
}

void SerialDevice::close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

}  // namespace sensord::devices
