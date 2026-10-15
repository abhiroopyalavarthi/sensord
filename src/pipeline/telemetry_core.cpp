#include "pipeline/telemetry_core.hpp"

#include <stdexcept>

#include "protocol/messages.hpp"

namespace sensord::pipeline {

TelemetryCore::TelemetryCore(CoreOptions opts)
    : opts_(opts), input_(opts.input_queue_capacity), hub_(input_) {}

TelemetryCore::~TelemetryCore() { stop(); }

uint32_t TelemetryCore::add_device(std::unique_ptr<devices::Device> dev) {
  if (started_) throw std::logic_error("add_device after start");
  const auto id = static_cast<uint32_t>(readers_.size());
  readers_.push_back(std::make_unique<DeviceReader>(id, std::move(dev), input_, opts_.reader));
  return id;
}

void TelemetryCore::start() {
  if (started_) return;
  started_ = true;
  hub_.start();
  for (auto& r : readers_) r->start();
}

void TelemetryCore::stop() {
  if (!started_ || stopped_) return;
  stopped_ = true;
  for (auto& r : readers_) r->stop();
  input_.close();
  hub_.stop();
}

std::vector<DeviceStatus> TelemetryCore::list_devices() const {
  std::vector<DeviceStatus> out;
  out.reserve(readers_.size());
  for (const auto& r : readers_) out.push_back(r->status());
  return out;
}

std::optional<std::string> TelemetryCore::set_sample_rate(uint32_t device_id, uint32_t hz) {
  if (device_id >= readers_.size()) return "unknown device id " + std::to_string(device_id);
  if (hz < protocol::kMinRateHz || hz > protocol::kMaxRateHz)
    return "rate must be between " + std::to_string(protocol::kMinRateHz) + " and " +
           std::to_string(protocol::kMaxRateHz) + " Hz";
  readers_[device_id]->request_sample_rate(hz);
  return std::nullopt;
}

std::shared_ptr<Subscription> TelemetryCore::subscribe(std::vector<uint32_t> device_ids,
                                                       std::size_t capacity) {
  return hub_.subscribe(std::move(device_ids), capacity);
}

void TelemetryCore::unsubscribe(const std::shared_ptr<Subscription>& sub) {
  hub_.unsubscribe(sub);
}

}  // namespace sensord::pipeline
