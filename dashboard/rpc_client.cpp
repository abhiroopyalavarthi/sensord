#include "rpc_client.hpp"

#include <grpcpp/grpcpp.h>

#include <chrono>

#include "telemetry.grpc.pb.h"

namespace v1 = sensord::v1;
using namespace std::chrono;

RpcClient::RpcClient(QObject* parent) : QObject(parent) {
  qRegisterMetaType<ChannelChunk>();
  qRegisterMetaType<ChunkList>();
  qRegisterMetaType<DeviceRow>();
  qRegisterMetaType<DeviceRows>();
}

RpcClient::~RpcClient() { disconnectFromServer(); }

void RpcClient::connectTo(const QString& address) {
  disconnectFromServer();
  const std::string addr = address.toStdString();
  stream_ = std::jthread([this, addr](std::stop_token st) { streamLoop(st, addr); });
  control_ = std::jthread([this, addr](std::stop_token st) { controlLoop(st, addr); });
}

void RpcClient::disconnectFromServer() {
  // jthread: request_stop() + join(). The stop_callbacks below cancel any
  // blocking gRPC call so the joins return quickly.
  stream_ = {};
  control_ = {};
}

void RpcClient::requestSampleRate(quint32 device, quint32 hz) {
  {
    std::lock_guard lk(mu_);
    pending_rates_.emplace_back(device, hz);
  }
  cv_.notify_all();
}

void RpcClient::streamLoop(std::stop_token st, std::string address) {
  auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
  auto stub = v1::Telemetry::NewStub(channel);
  milliseconds backoff(200);
  quint64 batches = 0;

  while (!st.stop_requested()) {
    grpc::ClientContext ctx;
    // If the window closes while we're blocked in Read(), cancel the call.
    std::stop_callback cancel(st, [&ctx] { ctx.TryCancel(); });
    v1::SubscribeRequest req;
    req.set_max_queue(512);
    auto reader = stub->Subscribe(&ctx, req);

    ChunkList pending;
    auto last_emit = steady_clock::now();
    v1::SampleBatch b;
    bool got_any = false;
    while (reader->Read(&b)) {
      if (!got_any) {
        got_any = true;
        backoff = milliseconds(200);
        emit serverStateChanged(true, QStringLiteral("streaming from %1").arg(QString::fromStdString(address)));
      }
      ++batches;
      for (const auto& blk : b.blocks()) {
        ChannelChunk c;
        c.device = b.device_id();
        c.channel = blk.channel();
        c.t0_us = blk.t0_us();
        c.period_us = blk.period_us();
        c.values = QVector<float>(blk.values().begin(), blk.values().end());
        pending.push_back(std::move(c));
      }
      // ~30 updates per second is plenty for the eye and keeps the GUI
      // event queue from flooding at high sample rates.
      const auto now = steady_clock::now();
      if (now - last_emit >= 33ms) {
        emit samplesReceived(std::move(pending));
        emit streamStats(batches, b.subscriber_dropped());
        pending = {};
        last_emit = now;
      }
    }
    auto status = reader->Finish();
    if (st.stop_requested()) break;
    emit serverStateChanged(false, QStringLiteral("stream down: %1 (retrying)")
                                       .arg(QString::fromStdString(status.error_message())));
    std::mutex m;
    std::unique_lock lk(m);
    std::condition_variable_any().wait_for(lk, st, backoff, [] { return false; });
    backoff = std::min(backoff * 2, milliseconds(3000));
  }
}

void RpcClient::controlLoop(std::stop_token st, std::string address) {
  auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
  auto stub = v1::Telemetry::NewStub(channel);

  while (!st.stop_requested()) {
    // Send queued rate changes first.
    std::deque<std::pair<quint32, quint32>> todo;
    {
      std::lock_guard lk(mu_);
      todo.swap(pending_rates_);
    }
    for (auto [dev, hz] : todo) {
      grpc::ClientContext ctx;
      ctx.set_deadline(system_clock::now() + 2s);
      v1::SetSampleRateRequest req;
      req.set_device_id(dev);
      req.set_sample_rate_hz(hz);
      v1::SetSampleRateResponse resp;
      auto s = stub->SetSampleRate(&ctx, req, &resp);
      emit rateResult(dev, s.ok(), s.ok() ? QStringLiteral("device %1 -> %2 Hz").arg(dev).arg(hz)
                                          : QString::fromStdString(s.error_message()));
    }

    grpc::ClientContext ctx;
    ctx.set_deadline(system_clock::now() + 1s);
    v1::ListDevicesResponse resp;
    if (stub->ListDevices(&ctx, {}, &resp).ok()) {
      DeviceRows rows;
      for (const auto& d : resp.devices()) {
        DeviceRow r;
        r.id = d.id();
        r.name = QString::fromStdString(d.name());
        r.kind = QString::fromStdString(d.kind());
        r.connected = d.connected();
        r.rate_hz = d.sample_rate_hz();
        r.frames = d.frames();
        r.crc_errors = d.crc_errors();
        r.frames_lost = d.frames_lost();
        r.reconnects = d.reconnects();
        rows.push_back(r);
      }
      emit devicesUpdated(rows);
    }

    std::unique_lock lk(mu_);
    cv_.wait_for(lk, st, 500ms, [this] { return !pending_rates_.empty(); });
  }
}
