#include <gtest/gtest.h>

#include <thread>

#include "devices/simulated_device.hpp"
#include "devices/tcp_device.hpp"
#include "pipeline/telemetry_core.hpp"
#include "test_util.hpp"

using namespace sensord;
using namespace std::chrono_literals;

namespace {

std::unique_ptr<devices::Device> sim(uint32_t rate = 1000, uint32_t seed = 1) {
  devices::SimConfig c;
  c.rate_hz = rate;
  c.seed = seed;
  return std::make_unique<devices::SimulatedDevice>("sim" + std::to_string(seed), c);
}

template <class Pred>
bool wait_until(Pred p, std::chrono::milliseconds timeout = 3000ms) {
  auto end = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < end) {
    if (p()) return true;
    std::this_thread::sleep_for(5ms);
  }
  return p();
}

pipeline::CoreOptions fast_opts() {
  pipeline::CoreOptions o;
  o.reader.backoff_initial = 20ms;
  o.reader.backoff_max = 200ms;
  return o;
}

}  // namespace

TEST(Pipeline, DeliversBatchesInOrder) {
  pipeline::TelemetryCore core(fast_opts());
  core.add_device(sim(1000));
  auto sub = core.subscribe({});
  core.start();

  uint64_t samples = 0;
  std::optional<uint32_t> last_seq;
  auto end = std::chrono::steady_clock::now() + 300ms;
  while (std::chrono::steady_clock::now() < end) {
    auto b = sub->next(50ms);
    if (!b) continue;
    EXPECT_EQ((*b)->device_id, 0u);
    for (auto& blk : (*b)->blocks) {
      if (last_seq) EXPECT_EQ(blk.frame_seq, *last_seq + 1);
      last_seq = blk.frame_seq;
      samples += blk.values.size();
    }
  }
  EXPECT_GT(samples, 200u);
  auto st = core.list_devices().at(0);
  EXPECT_TRUE(st.connected);
  EXPECT_EQ(st.sample_rate_hz, 1000u);
  EXPECT_EQ(st.frames_lost, 0u);
  core.stop();
  EXPECT_TRUE(sub->closed());
}

TEST(Pipeline, FilterOnlyDeliversRequestedDevices) {
  pipeline::TelemetryCore core(fast_opts());
  core.add_device(sim(500, 1));
  core.add_device(sim(500, 2));
  core.add_device(sim(500, 3));
  auto sub = core.subscribe({1});
  core.start();
  int got = 0;
  for (int i = 0; i < 20; ++i) {
    if (auto b = sub->next(100ms)) {
      EXPECT_EQ((*b)->device_id, 1u);
      ++got;
    }
  }
  EXPECT_GT(got, 5);
}

TEST(Pipeline, SlowSubscriberDropsOldestWithoutStallingOthers) {
  pipeline::TelemetryCore core(fast_opts());
  core.add_device(sim(2000, 1));
  core.add_device(sim(2000, 2));
  auto slow = core.subscribe({}, /*capacity=*/4);  // never reads
  auto fast = core.subscribe({}, 1024);
  core.start();

  std::map<uint32_t, uint32_t> next_seq;
  uint64_t gaps = 0, batches = 0;
  auto end = std::chrono::steady_clock::now() + 500ms;
  while (std::chrono::steady_clock::now() < end) {
    auto b = fast->next(50ms);
    if (!b) continue;
    ++batches;
    for (auto& blk : (*b)->blocks) {
      auto it = next_seq.find((*b)->device_id);
      if (it != next_seq.end() && it->second != blk.frame_seq) ++gaps;
      next_seq[(*b)->device_id] = blk.frame_seq + 1;
    }
  }
  EXPECT_GT(batches, 40u);
  EXPECT_EQ(gaps, 0u);
  EXPECT_EQ(fast->dropped(), 0u);
  EXPECT_GT(slow->dropped(), 10u);
  for (auto& st : core.list_devices()) EXPECT_EQ(st.frames_lost, 0u);
}

TEST(Pipeline, SetSampleRateReachesDevice) {
  pipeline::TelemetryCore core(fast_opts());
  core.add_device(sim(250));
  core.start();
  ASSERT_TRUE(wait_until([&] { return core.list_devices()[0].sample_rate_hz == 250; }));
  EXPECT_FALSE(core.set_sample_rate(0, 1000));
  EXPECT_TRUE(wait_until([&] { return core.list_devices()[0].sample_rate_hz == 1000; }));
  EXPECT_TRUE(core.set_sample_rate(7, 100)) << "unknown device must be rejected";
  EXPECT_TRUE(core.set_sample_rate(0, 0)) << "zero rate must be rejected";
  EXPECT_TRUE(core.set_sample_rate(0, 1'000'000)) << "huge rate must be rejected";
}

TEST(Pipeline, NoBatchingSendsOneBlockPerBatch) {
  auto o = fast_opts();
  o.reader.batching = false;
  pipeline::TelemetryCore core(o);
  core.add_device(sim(1000));
  auto sub = core.subscribe({});
  core.start();
  for (int i = 0; i < 20; ++i) {
    auto b = sub->next(200ms);
    ASSERT_TRUE(b);
    EXPECT_EQ((*b)->blocks.size(), 1u);
  }
}

TEST(Pipeline, ReconnectsAfterTcpDeviceUnplug) {
  devices::SimConfig cfg;
  cfg.rate_hz = 1000;
  test::TcpSimServer srv(cfg);
  pipeline::TelemetryCore core(fast_opts());
  core.add_device(std::make_unique<devices::TcpDevice>("127.0.0.1", srv.port()));
  core.start();

  ASSERT_TRUE(wait_until([&] { return core.list_devices()[0].samples > 100; }));
  EXPECT_EQ(core.list_devices()[0].connection_epoch, 1u);

  srv.kick();  // unplug
  ASSERT_TRUE(wait_until([&] { return core.list_devices()[0].connection_epoch >= 2; }));
  const auto before = core.list_devices()[0].samples;
  ASSERT_TRUE(wait_until([&] { return core.list_devices()[0].samples > before + 100; }));
  auto st = core.list_devices()[0];
  EXPECT_TRUE(st.connected);
  EXPECT_GE(st.reconnects, 1u);
  EXPECT_GE(srv.accepted(), 2);
}

TEST(Pipeline, MissingDeviceBacksOffAndStopIsPrompt) {
  uint16_t dead_port;
  {
    test::TcpSimServer tmp({});
    dead_port = tmp.port();
  }
  pipeline::CoreOptions o;
  o.reader.backoff_initial = 50ms;
  o.reader.backoff_max = 10s;  // long backoff: stop() must still interrupt it
  pipeline::TelemetryCore core(o);
  core.add_device(std::make_unique<devices::TcpDevice>("127.0.0.1", dead_port));
  core.start();
  std::this_thread::sleep_for(400ms);
  auto st = core.list_devices()[0];
  EXPECT_FALSE(st.connected);
  EXPECT_FALSE(st.last_error.empty());

  auto t0 = std::chrono::steady_clock::now();
  core.stop();
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 500ms);
}

TEST(Pipeline, StopWakesSubscriberBlockedInNext) {
  pipeline::TelemetryCore core(fast_opts());
  core.add_device(sim(100));
  auto sub = core.subscribe({});
  core.start();
  std::atomic<bool> done{false};
  std::jthread consumer([&] {
    // Mimics a gRPC handler: loop until the subscription closes.
    while (true) {
      auto b = sub->next(10s);
      if (!b && sub->closed()) break;
    }
    done = true;
  });
  std::this_thread::sleep_for(50ms);
  core.stop();
  consumer.join();
  EXPECT_TRUE(done.load());
}

TEST(Pipeline, SubscribeAfterStopGetsClosedSubscription) {
  pipeline::TelemetryCore core(fast_opts());
  core.add_device(sim());
  core.start();
  core.stop();
  auto sub = core.subscribe({});
  EXPECT_TRUE(sub->closed());
  EXPECT_FALSE(sub->next(10ms));
}
