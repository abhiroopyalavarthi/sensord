#pragma once
// Owns the whole data path: device readers -> input queue -> hub -> subscribers.
// The gRPC layer only talks to this class.

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "devices/device.hpp"
#include "pipeline/device_reader.hpp"
#include "pipeline/hub.hpp"

namespace sensord::pipeline {

struct CoreOptions {
  std::size_t input_queue_capacity = 1024;
  ReaderOptions reader;
};

class TelemetryCore {
 public:
  explicit TelemetryCore(CoreOptions opts = {});
  ~TelemetryCore();

  // Devices must be added before start().
  uint32_t add_device(std::unique_ptr<devices::Device> dev);

  void start();
  // Shutdown order matters: readers first (no more producers), then close the
  // input queue, then the hub (which closes every subscription so blocked
  // gRPC handlers wake up).
  void stop();

  std::vector<DeviceStatus> list_devices() const;
  // Returns an error message, or nullopt on success.
  std::optional<std::string> set_sample_rate(uint32_t device_id, uint32_t hz);

  std::shared_ptr<Subscription> subscribe(std::vector<uint32_t> device_ids,
                                          std::size_t capacity = 256);
  void unsubscribe(const std::shared_ptr<Subscription>& sub);

  Hub& hub() { return hub_; }

 private:
  CoreOptions opts_;
  BoundedQueue<BatchPtr> input_;
  Hub hub_;
  std::vector<std::unique_ptr<DeviceReader>> readers_;
  bool started_ = false;
  bool stopped_ = false;
};

}  // namespace sensord::pipeline
