#pragma once
// Device-side simulator logic. Produces the exact byte stream a real sensor
// board would: framed PPG-like samples, plus optional garbage bytes and
// corrupted frames. Shared by the in-process SimulatedDevice and by the
// standalone sensord-sim program (which serves it over TCP or a pty).

#include <chrono>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

#include "protocol/frame.hpp"

namespace sensord::devices {

struct SimConfig {
  uint16_t channels = 2;         // e.g. red + IR
  uint32_t rate_hz = 250;        // samples per second per channel
  uint32_t frame_interval_ms = 10;
  double heart_rate_bpm = 72.0;
  double noise = 0.02;           // std dev of additive noise
  double garbage_prob = 0.0;     // chance per frame of junk bytes before it
  double corrupt_prob = 0.0;     // chance per frame of one flipped byte
  double disconnect_mean_s = 0;  // mean time between unplugs (0 = never)
  double reconnect_delay_s = 1.0;
  uint32_t seed = 1;
};

class SignalGenerator {
 public:
  using Clock = std::chrono::steady_clock;

  explicit SignalGenerator(SimConfig cfg);

  // Appends frames for every sample that is due at `now`.
  void generate(Clock::time_point now, std::vector<uint8_t>& out);

  // Bytes from the host (command frames). Replies are appended to `out`.
  void on_host_bytes(std::span<const uint8_t> data, std::vector<uint8_t>& out);

  // When the next frame is due.
  Clock::time_point next_due() const;

  uint32_t rate_hz() const { return cfg_.rate_hz; }
  const SimConfig& config() const { return cfg_; }
  uint32_t next_seq() const { return seq_; }

 private:
  float sample_value(uint16_t ch, double t_s);
  void emit_frame(std::span<const uint8_t> payload, std::vector<uint8_t>& out);

  SimConfig cfg_;
  std::mt19937 rng_;
  std::normal_distribution<float> noise_;
  std::uniform_real_distribution<double> uni_{0.0, 1.0};
  protocol::FrameDecoder host_decoder_;
  std::vector<protocol::Frame> host_frames_;
  std::vector<uint8_t> scratch_;

  uint32_t seq_ = 0;
  uint64_t next_sample_us_ = 0;  // steady-clock time of the next sample
  Clock::time_point last_emit_{};
};

int64_t steady_now_us();

}  // namespace sensord::devices
