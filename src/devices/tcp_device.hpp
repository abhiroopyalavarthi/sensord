#pragma once
// TCP device link (plain TCP/IP socket, over Ethernet or Wi-Fi).

#include <cstdint>
#include <string>

#include "devices/device.hpp"

namespace sensord::devices {

class TcpDevice final : public Device {
 public:
  TcpDevice(std::string host, uint16_t port);
  ~TcpDevice() override;

  std::string name() const override { return host_ + ":" + std::to_string(port_); }
  std::string kind() const override { return "tcp"; }
  bool open(std::string& err) override;
  std::ptrdiff_t read(std::span<uint8_t> buf, std::chrono::milliseconds timeout) override;
  bool write(std::span<const uint8_t> data) override;
  void close() override;

 private:
  std::string host_;
  uint16_t port_;
  int fd_ = -1;
};

}  // namespace sensord::devices
