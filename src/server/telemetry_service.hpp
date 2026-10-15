#pragma once

#include <atomic>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "pipeline/telemetry_core.hpp"
#include "telemetry.grpc.pb.h"

namespace sensord::server {

class TelemetryServiceImpl final : public sensord::v1::Telemetry::Service {
 public:
  explicit TelemetryServiceImpl(pipeline::TelemetryCore& core) : core_(core) {}

  grpc::Status ListDevices(grpc::ServerContext*, const sensord::v1::ListDevicesRequest*,
                           sensord::v1::ListDevicesResponse* resp) override;

  grpc::Status Subscribe(grpc::ServerContext* ctx, const sensord::v1::SubscribeRequest* req,
                         grpc::ServerWriter<sensord::v1::SampleBatch>* writer) override;

  grpc::Status SetSampleRate(grpc::ServerContext*, const sensord::v1::SetSampleRateRequest* req,
                             sensord::v1::SetSampleRateResponse* resp) override;

  // Makes every open Subscribe call return promptly.
  void shutdown() { shutting_down_.store(true); }

  int active_streams() const { return active_streams_.load(); }

 private:
  pipeline::TelemetryCore& core_;
  std::atomic<bool> shutting_down_{false};
  std::atomic<int> active_streams_{0};
};

// Convenience wrapper: core + service + grpc::Server.
class Server {
 public:
  Server(pipeline::TelemetryCore& core, const std::string& listen_addr);
  ~Server();

  int port() const { return port_; }
  void stop();  // idempotent

 private:
  TelemetryServiceImpl service_;
  std::unique_ptr<grpc::Server> server_;
  int port_ = 0;
  bool stopped_ = false;
};

}  // namespace sensord::server
