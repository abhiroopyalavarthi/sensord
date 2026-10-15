// sensord-bench: load generator.
//
// Runs the full stack in one process: N simulated devices -> pipeline -> gRPC
// server on 127.0.0.1 -> M gRPC client threads (each on its own connection).
// Reports throughput and end-to-end latency, where latency is measured from
// the moment a sample was "taken" on the simulated device (its timestamp) to
// the moment the client decoded it. Both sides use the same monotonic clock.
//
//   sensord-bench --devices 16 --rate 2000 --subscribers 4 --seconds 10
//   sensord-bench ... --no-batching      (one gRPC message per frame)

#include <grpcpp/grpcpp.h>
#include <sys/resource.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "devices/simulated_device.hpp"
#include "pipeline/telemetry_core.hpp"
#include "server/telemetry_service.hpp"
#include "telemetry.grpc.pb.h"

using namespace sensord;
using namespace std::chrono;
namespace v1 = sensord::v1;

namespace {

struct Args {
  int devices = 16;
  uint32_t rate = 1000;
  uint16_t channels = 2;
  int subscribers = 4;
  double seconds = 10;
  double warmup = 1;
  int flush_ms = 10;
  bool batching = true;
};

int64_t now_us() { return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count(); }

double cpu_seconds() {
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  return double(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) +
         double(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1e-6;
}

struct SubResult {
  uint64_t samples = 0;
  uint64_t messages = 0;
  uint64_t dropped = 0;
  std::vector<uint32_t> latency_us;  // sampled: one per block (its last sample)
};

double pct(std::vector<uint32_t>& v, double p) {
  if (v.empty()) return 0;
  const std::size_t k = std::min(v.size() - 1, static_cast<std::size_t>(p * double(v.size())));
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
  return v[k];
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string_view s = argv[i];
    auto next = [&] { return std::string(i + 1 < argc ? argv[++i] : "0"); };
    if (s == "--devices") a.devices = std::stoi(next());
    else if (s == "--rate") a.rate = static_cast<uint32_t>(std::stoul(next()));
    else if (s == "--channels") a.channels = static_cast<uint16_t>(std::stoul(next()));
    else if (s == "--subscribers") a.subscribers = std::stoi(next());
    else if (s == "--seconds") a.seconds = std::stod(next());
    else if (s == "--warmup") a.warmup = std::stod(next());
    else if (s == "--flush-ms") a.flush_ms = std::stoi(next());
    else if (s == "--no-batching") a.batching = false;
    else {
      std::fprintf(stderr,
                   "usage: sensord-bench [--devices N] [--rate HZ] [--channels N] [--subscribers N]\n"
                   "                     [--seconds S] [--warmup S] [--flush-ms MS] [--no-batching]\n");
      return 2;
    }
  }

  pipeline::CoreOptions opts;
  opts.reader.batching = a.batching;
  opts.reader.flush_interval = milliseconds(a.flush_ms);
  pipeline::TelemetryCore core(opts);
  for (int i = 0; i < a.devices; ++i) {
    devices::SimConfig c;
    c.rate_hz = a.rate;
    c.channels = a.channels;
    c.seed = static_cast<uint32_t>(i + 1);
    core.add_device(std::make_unique<devices::SimulatedDevice>("sim" + std::to_string(i), c));
  }
  core.start();
  server::Server srv(core, "127.0.0.1:0");
  const std::string target = "127.0.0.1:" + std::to_string(srv.port());

  std::atomic<bool> measuring{false}, done{false};
  std::vector<SubResult> results(static_cast<std::size_t>(a.subscribers));
  std::vector<std::unique_ptr<grpc::ClientContext>> ctxs;
  for (int s = 0; s < a.subscribers; ++s) ctxs.push_back(std::make_unique<grpc::ClientContext>());

  std::vector<std::jthread> clients;
  for (int s = 0; s < a.subscribers; ++s) {
    clients.emplace_back([&, s] {
      grpc::ChannelArguments ch_args;
      ch_args.SetInt(GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL, 1);  // own TCP connection each
      ch_args.SetInt("sensord.client_id", s);
      auto ch = grpc::CreateCustomChannel(target, grpc::InsecureChannelCredentials(), ch_args);
      auto stub = v1::Telemetry::NewStub(ch);
      v1::SubscribeRequest req;
      req.set_max_queue(1024);
      auto reader = stub->Subscribe(ctxs[static_cast<std::size_t>(s)].get(), req);
      auto& r = results[static_cast<std::size_t>(s)];
      r.latency_us.reserve(1 << 20);
      v1::SampleBatch b;
      uint64_t dropped_at_start = 0;
      bool started = false;
      while (reader->Read(&b)) {
        if (!measuring.load(std::memory_order_relaxed)) continue;
        if (!started) {
          started = true;
          dropped_at_start = b.subscriber_dropped();
        }
        const int64_t t = now_us();
        ++r.messages;
        for (const auto& blk : b.blocks()) {
          const auto n = static_cast<uint64_t>(blk.values_size());
          if (n == 0) continue;
          r.samples += n;
          const int64_t newest_sample_us = static_cast<int64_t>(blk.t0_us() + (n - 1) * blk.period_us());
          r.latency_us.push_back(static_cast<uint32_t>(std::max<int64_t>(0, t - newest_sample_us)));
        }
        r.dropped = b.subscriber_dropped() - dropped_at_start;
        if (done.load(std::memory_order_relaxed)) break;
      }
      ctxs[static_cast<std::size_t>(s)]->TryCancel();
      reader->Finish();
    });
  }

  std::this_thread::sleep_for(duration<double>(a.warmup));
  uint64_t ingest0 = 0;
  for (auto& d : core.list_devices()) ingest0 += d.samples;
  const double cpu0 = cpu_seconds();
  const auto t0 = steady_clock::now();
  measuring = true;

  std::this_thread::sleep_for(duration<double>(a.seconds));

  done = true;
  const double elapsed = duration<double>(steady_clock::now() - t0).count();
  const double cpu = cpu_seconds() - cpu0;
  uint64_t ingest1 = 0;
  for (auto& d : core.list_devices()) ingest1 += d.samples;
  for (auto& c : ctxs) c->TryCancel();
  clients.clear();
  srv.stop();
  core.stop();

  uint64_t samples = 0, msgs = 0, dropped = 0;
  std::vector<uint32_t> lat;
  for (auto& r : results) {
    samples += r.samples;
    msgs += r.messages;
    dropped += r.dropped;
    lat.insert(lat.end(), r.latency_us.begin(), r.latency_us.end());
  }

  const double offered = double(a.devices) * a.rate * a.channels;
  std::printf("config: devices=%d rate=%u Hz channels=%u subscribers=%d batching=%s flush=%d ms\n",
              a.devices, a.rate, a.channels, a.subscribers, a.batching ? "on" : "off", a.flush_ms);
  std::printf("offered load      : %.0f samples/s\n", offered);
  std::printf("ingested          : %.0f samples/s\n", double(ingest1 - ingest0) / elapsed);
  std::printf("delivered (total) : %.0f samples/s across %d subscribers (%.0f per subscriber)\n",
              double(samples) / elapsed, a.subscribers, double(samples) / elapsed / a.subscribers);
  std::printf("gRPC messages     : %.0f msg/s\n", double(msgs) / elapsed);
  std::printf("subscriber drops  : %llu batches\n", (unsigned long long)dropped);
  std::printf("latency (sample taken -> client decoded): p50 %.2f ms  p99 %.2f ms  p99.9 %.2f ms  max %.2f ms\n",
              pct(lat, 0.50) / 1000.0, pct(lat, 0.99) / 1000.0, pct(lat, 0.999) / 1000.0,
              lat.empty() ? 0.0 : double(*std::max_element(lat.begin(), lat.end())) / 1000.0);
  std::printf("process CPU       : %.0f%% of one core\n", 100.0 * cpu / elapsed);
  return 0;
}
