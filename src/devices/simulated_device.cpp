#include "devices/simulated_device.hpp"

#include <algorithm>
#include <cstring>
#include <thread>

namespace sensord::devices {

using namespace std::chrono;

SimulatedDevice::SimulatedDevice(std::string name, SimConfig cfg)
    : name_(std::move(name)), gen_(cfg), rng_(cfg.seed * 7919u + 1u) {}

void SimulatedDevice::schedule_disconnect() {
  const double mean = gen_.config().disconnect_mean_s;
  if (mean <= 0) {
    disconnect_at_.reset();
    return;
  }
  std::exponential_distribution<double> d(1.0 / mean);
  disconnect_at_ = Clock::now() + duration_cast<Clock::duration>(duration<double>(d(rng_)));
}

bool SimulatedDevice::open(std::string& err) {
  if (Clock::now() < unplugged_until_) {
    err = name_ + ": device unplugged";
    return false;
  }
  open_ = true;
  pending_.clear();
  pending_off_ = 0;
  schedule_disconnect();
  return true;
}

std::ptrdiff_t SimulatedDevice::read(std::span<uint8_t> buf, milliseconds timeout) {
  if (!open_) return -1;

  if (pending_off_ == pending_.size()) {
    pending_.clear();
    pending_off_ = 0;
    const auto now = Clock::now();
    const auto due = gen_.next_due();
    if (due > now) {
      const auto wait = std::min<Clock::duration>(due - now, timeout);
      std::this_thread::sleep_for(wait);
    }
    gen_.generate(Clock::now(), pending_);
  }

  if (disconnect_at_ && Clock::now() >= *disconnect_at_) {
    // Simulated unplug: link drops mid-stream, comes back after a delay.
    open_ = false;
    unplugged_until_ =
        Clock::now() + duration_cast<Clock::duration>(
                           duration<double>(gen_.config().reconnect_delay_s));
    return -1;
  }

  const std::size_t avail = pending_.size() - pending_off_;
  if (avail == 0) return 0;
  const std::size_t n = std::min(avail, buf.size());
  std::memcpy(buf.data(), pending_.data() + pending_off_, n);
  pending_off_ += n;
  return static_cast<std::ptrdiff_t>(n);
}

bool SimulatedDevice::write(std::span<const uint8_t> data) {
  if (!open_) return false;
  // Replies (rate acks) go after whatever is already queued.
  gen_.on_host_bytes(data, pending_);
  return true;
}

void SimulatedDevice::close() {
  open_ = false;
  pending_.clear();
  pending_off_ = 0;
}

}  // namespace sensord::devices
