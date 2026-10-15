#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace sensord::pipeline {

// Samples from one frame of one channel.
struct SampleBlock {
  uint16_t channel = 0;
  uint32_t frame_seq = 0;
  uint64_t t0_us = 0;
  uint32_t period_us = 0;
  std::vector<float> values;
};

// Everything one device produced during one flush interval (10 ms default).
// Shared read-only between all subscribers, so fan-out never copies samples.
struct Batch {
  uint32_t device_id = 0;
  uint32_t connection_epoch = 0;
  uint64_t device_frames_lost = 0;  // cumulative at the time the batch closed
  int64_t host_rx_mono_ns = 0;
  std::vector<SampleBlock> blocks;
};

using BatchPtr = std::shared_ptr<const Batch>;

}  // namespace sensord::pipeline
