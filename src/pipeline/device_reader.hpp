#pragma once
// One reader thread per device. Owns the link, reconnects with exponential
// backoff, decodes frames, tracks sequence gaps, groups samples into batches
// and pushes them into the hub's input queue.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "devices/device.hpp"
#include "pipeline/batch.hpp"
#include "pipeline/bounded_queue.hpp"

namespace sensord::pipeline {

struct ReaderOptions {
  std::chrono::milliseconds flush_interval{10};
  bool batching = true;  // false = one batch per frame (to measure the difference)
  std::chrono::milliseconds backoff_initial{100};
  std::chrono::milliseconds backoff_max{5000};
  std::chrono::milliseconds read_timeout{20};
};

struct DeviceStatus {
  uint32_t id = 0;
  std::string name;
  std::string kind;
  bool connected = false;
  uint32_t sample_rate_hz = 0;
  uint64_t frames = 0;
  uint64_t crc_errors = 0;
  uint64_t frames_lost = 0;
  uint64_t reconnects = 0;
  uint64_t samples = 0;
  uint64_t bytes_skipped = 0;
  uint32_t connection_epoch = 0;
  std::string last_error;
};

class DeviceReader {
 public:
  DeviceReader(uint32_t id, std::unique_ptr<devices::Device> dev,
               BoundedQueue<BatchPtr>& out, ReaderOptions opts = {});
  ~DeviceReader();

  DeviceReader(const DeviceReader&) = delete;
  DeviceReader& operator=(const DeviceReader&) = delete;

  void start();
  void stop();  // request stop + join; safe to call twice

  // Queues a set-rate command; the reader thread sends it on its next loop.
  // The rate is remembered and re-sent after every reconnect.
  void request_sample_rate(uint32_t hz);

  DeviceStatus status() const;
  uint32_t id() const { return id_; }

 private:
  void run(std::stop_token st);
  bool sleep_backoff(std::stop_token st, std::chrono::milliseconds d);
  void flush(std::stop_token st);

  const uint32_t id_;
  std::unique_ptr<devices::Device> dev_;
  BoundedQueue<BatchPtr>& out_;
  const ReaderOptions opts_;
  const std::string name_;
  const std::string kind_;

  // Stats are written by the reader thread and read by anyone.
  std::atomic<bool> connected_{false};
  std::atomic<uint32_t> rate_hz_{0};
  std::atomic<uint64_t> frames_{0}, crc_errors_{0}, frames_lost_{0}, reconnects_{0},
      samples_{0}, bytes_skipped_{0};
  std::atomic<uint32_t> epoch_{0};

  mutable std::mutex mu_;  // guards pending_rate_, desired_rate_, last_error_
  std::optional<uint32_t> pending_rate_;
  std::optional<uint32_t> desired_rate_;
  std::string last_error_;

  // Reader-thread-only state.
  std::shared_ptr<Batch> batch_;
  std::chrono::steady_clock::time_point batch_started_{};

  std::mutex sleep_mu_;
  std::condition_variable_any sleep_cv_;

  std::jthread thread_;
};

}  // namespace sensord::pipeline
