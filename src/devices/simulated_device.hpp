#pragma once
// In-process simulated device. Behaves like a real link: bytes arrive in
// chunks, it can be "unplugged" for a while, and it answers set-rate commands.

#include <optional>
#include <random>
#include <string>
#include <vector>

#include "devices/device.hpp"
#include "devices/signal_generator.hpp"

namespace sensord::devices {

class SimulatedDevice final : public Device {
 public:
  SimulatedDevice(std::string name, SimConfig cfg);

  std::string name() const override { return name_; }
  std::string kind() const override { return "sim"; }
  bool open(std::string& err) override;
  std::ptrdiff_t read(std::span<uint8_t> buf, std::chrono::milliseconds timeout) override;
  bool write(std::span<const uint8_t> data) override;
  void close() override;

  const SignalGenerator& generator() const { return gen_; }

 private:
  using Clock = std::chrono::steady_clock;
  void schedule_disconnect();

  std::string name_;
  SignalGenerator gen_;
  std::mt19937 rng_;
  bool open_ = false;
  std::vector<uint8_t> pending_;
  std::size_t pending_off_ = 0;
  std::optional<Clock::time_point> disconnect_at_;
  Clock::time_point unplugged_until_{};
};

}  // namespace sensord::devices
