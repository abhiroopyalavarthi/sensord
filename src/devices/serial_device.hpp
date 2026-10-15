#pragma once
// USB serial link (CDC-ACM / FTDI / CP210x ...) using POSIX termios.
// On macOS use the /dev/cu.* node, on Linux /dev/ttyUSB* or /dev/ttyACM*.

#include <string>

#include "devices/device.hpp"

namespace sensord::devices {

class SerialDevice final : public Device {
 public:
  SerialDevice(std::string path, int baud = 115200);
  ~SerialDevice() override;

  std::string name() const override { return path_; }
  std::string kind() const override { return "serial"; }
  bool open(std::string& err) override;
  std::ptrdiff_t read(std::span<uint8_t> buf, std::chrono::milliseconds timeout) override;
  bool write(std::span<const uint8_t> data) override;
  void close() override;

 private:
  std::string path_;
  int baud_;
  int fd_ = -1;
};

}  // namespace sensord::devices
