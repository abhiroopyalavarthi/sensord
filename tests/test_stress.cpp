// Concurrency stress test. Meant to be run under ThreadSanitizer.
//
// 8 simulated devices (with garbage bytes, corrupted frames and random
// unplugs) feed the pipeline while 20 subscriber threads connect and
// disconnect at random, some reading slowly. A control thread changes sample
// rates and lists devices. At the end the core is stopped while subscribers
// are still attached.
//
// SENSORD_STRESS_SEED and SENSORD_STRESS_SECONDS pick the seed and duration.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <map>
#include <random>
#include <thread>

#include "devices/simulated_device.hpp"
#include "pipeline/telemetry_core.hpp"

using namespace sensord;
using namespace std::chrono_literals;

namespace {
uint32_t env_u32(const char* name, uint32_t def) {
  const char* v = std::getenv(name);
  return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10)) : def;
}
}  // namespace

TEST(Stress, RandomSubscribersDevicesAndShutdown) {
  const uint32_t seed = env_u32("SENSORD_STRESS_SEED", 1);
  const uint32_t seconds = env_u32("SENSORD_STRESS_SECONDS", 3);
  std::printf("stress: seed=%u seconds=%u\n", seed, seconds);

  constexpr int kDevices = 8;
  constexpr int kSubscribers = 20;

  pipeline::CoreOptions opts;
  opts.input_queue_capacity = 64;  // small, so backpressure actually happens
  opts.reader.backoff_initial = 10ms;
  opts.reader.backoff_max = 100ms;
  pipeline::TelemetryCore core(opts);

  std::mt19937 rng(seed);
  for (int i = 0; i < kDevices; ++i) {
    devices::SimConfig c;
    c.rate_hz = 500 + 250 * static_cast<uint32_t>(i % 4);
    c.channels = static_cast<uint16_t>(1 + i % 3);
    c.garbage_prob = 0.02;
    c.corrupt_prob = 0.02;
    c.disconnect_mean_s = 0.4 + 0.1 * i;
    c.reconnect_delay_s = 0.05;
    c.seed = seed * 100 + static_cast<uint32_t>(i);
    core.add_device(std::make_unique<devices::SimulatedDevice>("sim" + std::to_string(i), c));
  }
  core.start();

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> total_samples{0}, total_subs{0}, order_violations{0};

  std::vector<std::jthread> subs;
  for (int s = 0; s < kSubscribers; ++s) {
    subs.emplace_back([&, s] {
      std::mt19937 r(seed * 1000 + static_cast<uint32_t>(s));
      while (!stop.load()) {
        std::vector<uint32_t> filter;
        if (r() % 3 == 0)
          for (int d = 0; d < kDevices; ++d)
            if (r() % 2) filter.push_back(static_cast<uint32_t>(d));
        const std::size_t cap = 1 + r() % 64;
        const bool slow = r() % 4 == 0;
        auto sub = core.subscribe(filter, cap);
        total_subs.fetch_add(1);

        // Per (device, epoch), frame sequence numbers must strictly increase,
        // drops or not.
        std::map<uint32_t, std::pair<uint32_t, uint32_t>> last;  // dev -> (epoch, seq)
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(20 + r() % 300);
        while (!stop.load() && std::chrono::steady_clock::now() < until) {
          auto b = sub->next(20ms);
          if (!b) {
            if (sub->closed()) break;
            continue;
          }
          for (auto& blk : (*b)->blocks) {
            auto it = last.find((*b)->device_id);
            if (it != last.end() && it->second.first == (*b)->connection_epoch &&
                static_cast<int32_t>(blk.frame_seq - it->second.second) <= 0)
              order_violations.fetch_add(1);
            last[(*b)->device_id] = {(*b)->connection_epoch, blk.frame_seq};
            total_samples.fetch_add(blk.values.size(), std::memory_order_relaxed);
          }
          if (slow) std::this_thread::sleep_for(std::chrono::milliseconds(r() % 15));
        }
        // Sometimes leave without unsubscribing, to test hub cleanup on stop().
        if (r() % 5) core.unsubscribe(sub);
        std::this_thread::sleep_for(std::chrono::milliseconds(r() % 30));
      }
    });
  }

  std::jthread control([&] {
    std::mt19937 r(seed + 7);
    while (!stop.load()) {
      core.set_sample_rate(r() % kDevices, 100 + r() % 2000);
      auto list = core.list_devices();
      EXPECT_EQ(list.size(), static_cast<std::size_t>(kDevices));
      std::this_thread::sleep_for(std::chrono::milliseconds(5 + r() % 50));
    }
  });

  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  // Stop the core while subscribers are still attached and reading.
  core.stop();
  stop.store(true);
  subs.clear();
  control = {};

  uint64_t crc = 0, reconnects = 0;
  for (auto& st : core.list_devices()) {
    crc += st.crc_errors;
    reconnects += st.reconnects;
  }
  std::printf("stress: samples=%llu subscriptions=%llu crc_errors=%llu reconnects=%llu\n",
              (unsigned long long)total_samples.load(), (unsigned long long)total_subs.load(),
              (unsigned long long)crc, (unsigned long long)reconnects);
  EXPECT_GT(total_samples.load(), 1000u);
  EXPECT_GT(total_subs.load(), static_cast<uint64_t>(kSubscribers));
  EXPECT_GT(reconnects, 0u);
  EXPECT_EQ(order_violations.load(), 0u);
}
