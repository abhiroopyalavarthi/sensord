// sensorctl: command-line client for sensord.
//
//   sensorctl list
//   sensorctl subscribe [--device ID]... [--seconds S] [--raw]
//   sensorctl set-rate ID HZ
//   sensorctl check [--seconds S]     end-to-end sequence check (exit 1 on unexplained gaps)
//
// Global: --server ADDR (default localhost:50051)

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "telemetry.grpc.pb.h"

namespace v1 = sensord::v1;
using namespace std::chrono;

namespace {

void usage() {
  std::cerr << "usage: sensorctl [--server ADDR] <command>\n"
               "  list\n"
               "  subscribe [--device ID]... [--seconds S] [--raw]\n"
               "  set-rate ID HZ\n"
               "  check [--seconds S] [--device ID]...\n";
}

int cmd_list(v1::Telemetry::Stub& stub) {
  grpc::ClientContext ctx;
  ctx.set_deadline(system_clock::now() + 3s);
  v1::ListDevicesResponse resp;
  auto st = stub.ListDevices(&ctx, {}, &resp);
  if (!st.ok()) {
    std::cerr << "ListDevices failed: " << st.error_message() << "\n";
    return 1;
  }
  std::printf("%-3s %-28s %-7s %-6s %8s %10s %8s %8s %6s\n", "ID", "NAME", "KIND", "LINK",
              "RATE_HZ", "FRAMES", "CRC_ERR", "LOST", "RECON");
  for (const auto& d : resp.devices()) {
    std::printf("%-3u %-28s %-7s %-6s %8u %10llu %8llu %8llu %6llu\n", d.id(), d.name().c_str(),
                d.kind().c_str(), d.connected() ? "up" : "DOWN", d.sample_rate_hz(),
                (unsigned long long)d.frames(), (unsigned long long)d.crc_errors(),
                (unsigned long long)d.frames_lost(), (unsigned long long)d.reconnects());
  }
  return 0;
}

int cmd_set_rate(v1::Telemetry::Stub& stub, uint32_t id, uint32_t hz) {
  grpc::ClientContext ctx;
  ctx.set_deadline(system_clock::now() + 3s);
  v1::SetSampleRateRequest req;
  req.set_device_id(id);
  req.set_sample_rate_hz(hz);
  v1::SetSampleRateResponse resp;
  auto st = stub.SetSampleRate(&ctx, req, &resp);
  if (!st.ok()) {
    std::cerr << "SetSampleRate failed: " << st.error_message() << "\n";
    return 1;
  }
  std::cout << "device " << id << ": sample rate set to " << hz << " Hz\n";
  return 0;
}

struct DevTrack {
  bool have = false;
  uint32_t epoch = 0;
  uint32_t next_seq = 0;
  uint64_t frames_lost = 0;
  uint64_t samples = 0;
  uint64_t gaps_explained = 0;
  uint64_t gaps_unexplained = 0;
  std::map<uint32_t, float> last_value;
};

int cmd_stream(v1::Telemetry::Stub& stub, const std::vector<uint32_t>& ids, double seconds,
               bool raw, bool check) {
  grpc::ClientContext ctx;
  if (seconds > 0)
    ctx.set_deadline(system_clock::now() + duration_cast<system_clock::duration>(duration<double>(seconds)));
  v1::SubscribeRequest req;
  for (auto id : ids) req.add_device_ids(id);
  auto reader = stub.Subscribe(&ctx, req);

  std::map<uint32_t, DevTrack> devs;
  uint64_t last_dropped = 0;
  uint64_t batches = 0;
  auto last_print = steady_clock::now();
  std::map<uint32_t, uint64_t> samples_at_print;

  v1::SampleBatch b;
  while (reader->Read(&b)) {
    ++batches;
    auto& t = devs[b.device_id()];
    const bool dropped_more = b.subscriber_dropped() > last_dropped;
    const bool lost_more = b.device_frames_lost() > t.frames_lost;
    if (t.have && b.connection_epoch() != t.epoch) t.have = false;  // reconnected: new stream
    for (const auto& blk : b.blocks()) {
      if (t.have && blk.frame_seq() != t.next_seq) {
        if (dropped_more || lost_more) {
          ++t.gaps_explained;
        } else {
          ++t.gaps_unexplained;
          if (check)
            std::cerr << "UNEXPLAINED GAP dev " << b.device_id() << ": expected seq "
                      << t.next_seq << " got " << blk.frame_seq() << "\n";
        }
      }
      t.have = true;
      t.next_seq = blk.frame_seq() + 1;
      t.samples += static_cast<uint64_t>(blk.values_size());
      if (blk.values_size() > 0) t.last_value[blk.channel()] = blk.values(blk.values_size() - 1);
      if (raw)
        std::printf("dev %u ch %u seq %u t0_us %llu n %d first %.4f\n", b.device_id(),
                    blk.channel(), blk.frame_seq(), (unsigned long long)blk.t0_us(),
                    blk.values_size(), blk.values_size() ? blk.values(0) : 0.0f);
    }
    t.epoch = b.connection_epoch();
    t.frames_lost = b.device_frames_lost();
    last_dropped = b.subscriber_dropped();

    const auto now = steady_clock::now();
    if (!raw && !check && now - last_print >= 1s) {
      const double dt = duration<double>(now - last_print).count();
      for (auto& [id, d] : devs) {
        std::printf("dev %-3u %8.0f samples/s ", id, double(d.samples - samples_at_print[id]) / dt);
        for (auto& [ch, v] : d.last_value) std::printf(" ch%u=%7.3f", ch, v);
        std::printf("\n");
        samples_at_print[id] = d.samples;
      }
      std::printf("-- batches %llu, dropped for this client %llu\n", (unsigned long long)batches,
                  (unsigned long long)last_dropped);
      std::fflush(stdout);
      last_print = now;
    }
  }
  auto st = reader->Finish();
  if (!st.ok() && st.error_code() != grpc::StatusCode::DEADLINE_EXCEEDED) {
    std::cerr << "Subscribe ended: " << st.error_message() << "\n";
    if (batches == 0) return 1;
  }

  if (check) {
    uint64_t unexplained = 0, total = 0;
    for (auto& [id, d] : devs) {
      std::printf("dev %u: %llu samples, %llu explained gaps, %llu unexplained gaps\n", id,
                  (unsigned long long)d.samples, (unsigned long long)d.gaps_explained,
                  (unsigned long long)d.gaps_unexplained);
      unexplained += d.gaps_unexplained;
      total += d.samples;
    }
    std::printf("batches %llu, subscriber drops %llu\n", (unsigned long long)batches,
                (unsigned long long)last_dropped);
    if (total == 0) {
      std::cerr << "check FAILED: no samples received\n";
      return 1;
    }
    if (unexplained) {
      std::cerr << "check FAILED\n";
      return 1;
    }
    std::cout << "check OK\n";
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string server = "localhost:50051";
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--server" && i + 1 < argc) server = argv[++i];
    else args.emplace_back(argv[i]);
  }
  if (args.empty()) {
    usage();
    return 2;
  }

  auto channel = grpc::CreateChannel(server, grpc::InsecureChannelCredentials());
  auto stub = v1::Telemetry::NewStub(channel);
  const std::string cmd = args[0];

  try {
    if (cmd == "list") return cmd_list(*stub);
    if (cmd == "set-rate") {
      if (args.size() != 3) {
        usage();
        return 2;
      }
      return cmd_set_rate(*stub, static_cast<uint32_t>(std::stoul(args[1])),
                          static_cast<uint32_t>(std::stoul(args[2])));
    }
    if (cmd == "subscribe" || cmd == "check") {
      std::vector<uint32_t> ids;
      double seconds = cmd == "check" ? 5 : 0;
      bool raw = false;
      for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--device" && i + 1 < args.size()) ids.push_back(static_cast<uint32_t>(std::stoul(args[++i])));
        else if (args[i] == "--seconds" && i + 1 < args.size()) seconds = std::stod(args[++i]);
        else if (args[i] == "--raw") raw = true;
        else {
          usage();
          return 2;
        }
      }
      return cmd_stream(*stub, ids, seconds, raw, cmd == "check");
    }
  } catch (const std::exception& e) {
    std::cerr << "sensorctl: bad argument: " << e.what() << "\n";
    return 2;
  }
  usage();
  return 2;
}
