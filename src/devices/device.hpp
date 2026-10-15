#pragma once
// One interface for every kind of device link. The reader thread only ever
// talks to this, so it never knows whether bytes come from the simulator, a
// USB serial port or a TCP socket.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace sensord::devices {

class Device {
 public:
  virtual ~Device() = default;

  virtual std::string name() const = 0;
  virtual std::string kind() const = 0;  // "sim", "serial", "tcp"

  // Opens (or reopens) the link. On failure returns false and fills `err`.
  virtual bool open(std::string& err) = 0;

  // Reads up to buf.size() bytes. Returns >0 bytes read, 0 on timeout,
  // -1 if the link dropped (caller should close() and reconnect).
  virtual std::ptrdiff_t read(std::span<uint8_t> buf, std::chrono::milliseconds timeout) = 0;

  // Writes all bytes (used for command frames). False if the link dropped.
  virtual bool write(std::span<const uint8_t> data) = 0;

  virtual void close() = 0;
};

}  // namespace sensord::devices
