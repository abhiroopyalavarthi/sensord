#include "server/telemetry_service.hpp"

#include <chrono>
#include <stdexcept>

#include "server/convert.hpp"

namespace sensord::server {

using namespace std::chrono_literals;
namespace v1 = sensord::v1;

grpc::Status TelemetryServiceImpl::ListDevices(grpc::ServerContext*, const v1::ListDevicesRequest*,
                                               v1::ListDevicesResponse* resp) {
  for (const auto& s : core_.list_devices()) to_proto(s, resp->add_devices());
  return grpc::Status::OK;
}

grpc::Status TelemetryServiceImpl::Subscribe(grpc::ServerContext* ctx,
                                             const v1::SubscribeRequest* req,
                                             grpc::ServerWriter<v1::SampleBatch>* writer) {
  std::vector<uint32_t> ids(req->device_ids().begin(), req->device_ids().end());
  const std::size_t cap = req->max_queue() ? req->max_queue() : 256;
  auto sub = core_.subscribe(std::move(ids), cap);
  active_streams_.fetch_add(1);

  v1::SampleBatch msg;
  // Wake up every 100 ms to notice a cancelled client or a server shutdown.
  while (!ctx->IsCancelled() && !shutting_down_.load()) {
    auto b = sub->next(100ms);
    if (!b) {
      if (sub->closed()) break;
      continue;
    }
    msg.Clear();
    to_proto(**b, sub->dropped(), &msg);
    if (!writer->Write(msg)) break;  // client went away
  }

  core_.unsubscribe(sub);
  active_streams_.fetch_sub(1);
  return grpc::Status::OK;
}

grpc::Status TelemetryServiceImpl::SetSampleRate(grpc::ServerContext*,
                                                 const v1::SetSampleRateRequest* req,
                                                 v1::SetSampleRateResponse* resp) {
  if (auto err = core_.set_sample_rate(req->device_id(), req->sample_rate_hz())) {
    resp->set_accepted(false);
    resp->set_error(*err);
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, *err);
  }
  resp->set_accepted(true);
  return grpc::Status::OK;
}

Server::Server(pipeline::TelemetryCore& core, const std::string& listen_addr) : service_(core) {
  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen_addr, grpc::InsecureServerCredentials(), &port_);
  builder.RegisterService(&service_);
  server_ = builder.BuildAndStart();
  if (!server_ || port_ == 0) throw std::runtime_error("failed to listen on " + listen_addr);
}

Server::~Server() { stop(); }

void Server::stop() {
  if (stopped_) return;
  stopped_ = true;
  service_.shutdown();
  server_->Shutdown(std::chrono::system_clock::now() + 2s);
  server_->Wait();
}

}  // namespace sensord::server
