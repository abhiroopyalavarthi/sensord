// End-to-end: simulated devices -> pipeline -> real gRPC server on localhost
// -> real gRPC client. Checks that sequence numbers arrive in order with no
// gaps except ones the server reported.

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <map>
#include <thread>

#include "devices/simulated_device.hpp"
#include "pipeline/telemetry_core.hpp"
#include "server/telemetry_service.hpp"
#include "telemetry.grpc.pb.h"

using namespace sensord;
using namespace std::chrono_literals;
namespace v1 = sensord::v1;

class E2E : public ::testing::Test {
 protected:
  void SetUp() override {
    for (uint32_t i = 0; i < 3; ++i) {
      devices::SimConfig c;
      c.rate_hz = 1000;
      c.seed = i + 1;
      core_.add_device(std::make_unique<devices::SimulatedDevice>("sim" + std::to_string(i), c));
    }
    core_.start();
    server_ = std::make_unique<server::Server>(core_, "127.0.0.1:0");
    channel_ = grpc::CreateChannel("127.0.0.1:" + std::to_string(server_->port()),
                                   grpc::InsecureChannelCredentials());
    stub_ = v1::Telemetry::NewStub(channel_);
  }

  void TearDown() override {
    server_->stop();
    core_.stop();
  }

  pipeline::TelemetryCore core_;
  std::unique_ptr<server::Server> server_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<v1::Telemetry::Stub> stub_;
};

TEST_F(E2E, ListDevices) {
  grpc::ClientContext ctx;
  v1::ListDevicesResponse resp;
  ASSERT_TRUE(stub_->ListDevices(&ctx, {}, &resp).ok());
  ASSERT_EQ(resp.devices_size(), 3);
  EXPECT_EQ(resp.devices(1).name(), "sim1");
  EXPECT_EQ(resp.devices(1).kind(), "sim");
}

TEST_F(E2E, StreamHasNoUnexplainedGaps) {
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + 1500ms);
  auto reader = stub_->Subscribe(&ctx, {});

  struct Track {
    bool have = false;
    uint32_t next = 0, epoch = 0;
    uint64_t lost = 0, samples = 0;
  };
  std::map<uint32_t, Track> devs;
  uint64_t last_dropped = 0, unexplained = 0, batches = 0;
  v1::SampleBatch b;
  while (reader->Read(&b)) {
    ++batches;
    auto& t = devs[b.device_id()];
    const bool explained = b.subscriber_dropped() > last_dropped || b.device_frames_lost() > t.lost;
    if (t.have && t.epoch != b.connection_epoch()) t.have = false;
    for (auto& blk : b.blocks()) {
      if (t.have && blk.frame_seq() != t.next && !explained) ++unexplained;
      t.have = true;
      t.next = blk.frame_seq() + 1;
      t.samples += static_cast<uint64_t>(blk.values_size());
    }
    t.epoch = b.connection_epoch();
    t.lost = b.device_frames_lost();
    last_dropped = b.subscriber_dropped();
  }
  auto st = reader->Finish();
  EXPECT_EQ(st.error_code(), grpc::StatusCode::DEADLINE_EXCEEDED);
  EXPECT_EQ(unexplained, 0u);
  EXPECT_EQ(devs.size(), 3u);
  for (auto& [id, t] : devs) EXPECT_GT(t.samples, 1000u) << "device " << id;
  EXPECT_GT(batches, 100u);
}

TEST_F(E2E, SetSampleRateReachesDevice) {
  {
    grpc::ClientContext ctx;
    v1::SetSampleRateRequest req;
    req.set_device_id(2);
    req.set_sample_rate_hz(200);
    v1::SetSampleRateResponse resp;
    ASSERT_TRUE(stub_->SetSampleRate(&ctx, req, &resp).ok());
    EXPECT_TRUE(resp.accepted());
  }
  // The new period must show up in the stream for device 2.
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + 2s);
  v1::SubscribeRequest req;
  req.add_device_ids(2);
  auto reader = stub_->Subscribe(&ctx, req);
  v1::SampleBatch b;
  bool saw = false;
  while (!saw && reader->Read(&b)) {
    EXPECT_EQ(b.device_id(), 2u);
    for (auto& blk : b.blocks()) saw |= blk.period_us() == 5000;
  }
  ctx.TryCancel();
  reader->Finish();
  EXPECT_TRUE(saw);
}

TEST_F(E2E, SetSampleRateRejectsBadInput) {
  grpc::ClientContext ctx;
  v1::SetSampleRateRequest req;
  req.set_device_id(99);
  req.set_sample_rate_hz(100);
  v1::SetSampleRateResponse resp;
  auto st = stub_->SetSampleRate(&ctx, req, &resp);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(E2E, ClientCancelReleasesSubscription) {
  auto ctx = std::make_unique<grpc::ClientContext>();
  auto reader = stub_->Subscribe(ctx.get(), {});
  v1::SampleBatch b;
  ASSERT_TRUE(reader->Read(&b));
  EXPECT_EQ(core_.hub().subscriber_count(), 1u);
  ctx->TryCancel();
  while (reader->Read(&b)) {
  }
  reader->Finish();
  auto end = std::chrono::steady_clock::now() + 2s;
  while (core_.hub().subscriber_count() != 0 && std::chrono::steady_clock::now() < end)
    std::this_thread::sleep_for(10ms);
  EXPECT_EQ(core_.hub().subscriber_count(), 0u);
}

TEST_F(E2E, ServerShutdownEndsOpenStreams) {
  grpc::ClientContext ctx;
  auto reader = stub_->Subscribe(&ctx, {});
  v1::SampleBatch b;
  ASSERT_TRUE(reader->Read(&b));
  auto t0 = std::chrono::steady_clock::now();
  std::jthread stopper([&] { server_->stop(); });
  while (reader->Read(&b)) {
  }
  reader->Finish();
  stopper.join();
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 3s);
}
