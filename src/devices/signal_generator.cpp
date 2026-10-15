#include "devices/signal_generator.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "protocol/messages.hpp"

namespace sensord::devices {

using namespace std::chrono;
namespace proto = sensord::protocol;

int64_t steady_now_us() {
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

SignalGenerator::SignalGenerator(SimConfig cfg)
    : cfg_(cfg), rng_(cfg.seed), noise_(0.0f, static_cast<float>(cfg.noise)) {
  cfg_.rate_hz = std::clamp(cfg_.rate_hz, proto::kMinRateHz, proto::kMaxRateHz);
  cfg_.channels = std::max<uint16_t>(cfg_.channels, 1);
  next_sample_us_ = static_cast<uint64_t>(steady_now_us());
  last_emit_ = Clock::now();
}

SignalGenerator::Clock::time_point SignalGenerator::next_due() const {
  return last_emit_ + milliseconds(cfg_.frame_interval_ms);
}

// A rough PPG pulse: systolic peak + smaller dicrotic wave, slow respiratory
// baseline wander, and white noise. Good enough to look like a real signal.
float SignalGenerator::sample_value(uint16_t ch, double t_s) {
  const double beat = cfg_.heart_rate_bpm / 60.0;
  double phase = std::fmod(t_s * beat + 0.03 * ch, 1.0);
  auto bump = [](double x, double mu, double sigma) {
    const double d = (x - mu) / sigma;
    return std::exp(-0.5 * d * d);
  };
  const double pulse = bump(phase, 0.15, 0.05) + 0.35 * bump(phase, 0.45, 0.07);
  const double resp = 0.1 * std::sin(2.0 * std::numbers::pi * 0.25 * t_s);
  const double gain = 1.0 + 0.3 * ch;
  return static_cast<float>(gain * pulse + resp + 0.5 * ch) + noise_(rng_);
}

void SignalGenerator::emit_frame(std::span<const uint8_t> payload, std::vector<uint8_t>& out) {
  if (cfg_.garbage_prob > 0 && uni_(rng_) < cfg_.garbage_prob) {
    const int junk = 1 + static_cast<int>(uni_(rng_) * 24);
    for (int i = 0; i < junk; ++i) {
      // Throw in some fake sync bytes so the resync path really gets tested.
      const double r = uni_(rng_);
      out.push_back(r < 0.15 ? proto::kSync0 : r < 0.25 ? proto::kSync1
                                                        : static_cast<uint8_t>(rng_() & 0xFF));
    }
  }
  const std::size_t start = out.size();
  proto::encode_frame_into(out, seq_++, payload);
  if (cfg_.corrupt_prob > 0 && uni_(rng_) < cfg_.corrupt_prob) {
    const std::size_t len = out.size() - start;
    const std::size_t at = start + 2 + static_cast<std::size_t>(uni_(rng_) * double(len - 2));
    out[std::min(at, out.size() - 1)] ^= static_cast<uint8_t>(1u << (rng_() % 8));
  }
}

void SignalGenerator::generate(Clock::time_point now, std::vector<uint8_t>& out) {
  if (now < next_due()) return;
  last_emit_ = now;

  const uint64_t now_us =
      static_cast<uint64_t>(duration_cast<microseconds>(now.time_since_epoch()).count());
  const uint32_t period_us = std::max<uint32_t>(1, 1'000'000u / cfg_.rate_hz);
  if (now_us < next_sample_us_) return;

  uint64_t due = (now_us - next_sample_us_) / period_us + 1;
  // If we fell far behind (process was paused), skip ahead instead of
  // dumping seconds of backlog in one go.
  const uint64_t max_backlog = std::max<uint64_t>(cfg_.rate_hz, 1);  // 1 s
  if (due > max_backlog) {
    next_sample_us_ += (due - max_backlog) * period_us;
    due = max_backlog;
  }

  while (due > 0) {
    const auto count = static_cast<uint16_t>(std::min<uint64_t>(due, proto::kMaxSamplesPerMsg));
    for (uint16_t ch = 0; ch < cfg_.channels; ++ch) {
      proto::SamplesMsg m;
      m.channel = ch;
      m.t0_us = next_sample_us_;
      m.period_us = period_us;
      m.values.resize(count);
      for (uint16_t i = 0; i < count; ++i) {
        const double t_s = static_cast<double>(next_sample_us_ + uint64_t(i) * period_us) * 1e-6;
        m.values[i] = sample_value(ch, t_s);
      }
      scratch_.clear();
      proto::encode_message(scratch_, m);
      emit_frame(scratch_, out);
    }
    next_sample_us_ += uint64_t(count) * period_us;
    due -= count;
  }
}

void SignalGenerator::on_host_bytes(std::span<const uint8_t> data, std::vector<uint8_t>& out) {
  host_frames_.clear();
  host_decoder_.feed(data, host_frames_);
  for (const auto& f : host_frames_) {
    auto msg = proto::decode_message(f.payload);
    if (!msg) continue;
    if (auto* sr = std::get_if<proto::SetRateMsg>(&*msg)) {
      cfg_.rate_hz = std::clamp(sr->rate_hz, proto::kMinRateHz, proto::kMaxRateHz);
      scratch_.clear();
      proto::encode_message(scratch_, proto::RateAckMsg{cfg_.rate_hz});
      emit_frame(scratch_, out);
    }
  }
}

}  // namespace sensord::devices
