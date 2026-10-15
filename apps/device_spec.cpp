#include "device_spec.hpp"

#include <stdexcept>
#include <string_view>

#include "devices/serial_device.hpp"
#include "devices/simulated_device.hpp"
#include "devices/tcp_device.hpp"

namespace sensord::app {

std::string usage() {
  return R"(usage: sensord [options]

  --listen ADDR            gRPC listen address (default 0.0.0.0:50051)
  --sim N                  add N simulated devices
  --sim-rate HZ            simulated sample rate per channel (default 250)
  --sim-channels N         channels per simulated device (default 2)
  --sim-garbage P          chance per frame of junk bytes (default 0)
  --sim-corrupt P          chance per frame of a flipped bit (default 0)
  --sim-unplug-every S     mean seconds between simulated unplugs (default never)
  --serial PATH[@BAUD]     add a USB serial device, e.g. /dev/cu.usbserial-0001@115200
  --tcp HOST:PORT          add a TCP device, e.g. 127.0.0.1:9000
  --flush-ms MS            batch flush interval (default 10)
  --no-batching            send one gRPC message per frame (for comparison)
  --queue N                hub input queue capacity in batches (default 1024)
)";
}

namespace {
double to_double(std::string_view flag, const char* v) {
  try {
    return std::stod(v);
  } catch (...) {
    throw std::invalid_argument(std::string(flag) + ": not a number: " + v);
  }
}
}  // namespace

Options parse_args(int argc, char** argv) {
  Options o;
  auto need = [&](int& i) -> const char* {
    if (i + 1 >= argc) throw std::invalid_argument(std::string(argv[i]) + " needs a value");
    return argv[++i];
  };
  for (int i = 1; i < argc; ++i) {
    std::string_view a = argv[i];
    if (a == "--listen") o.listen = need(i);
    else if (a == "--sim") o.sim_count = static_cast<int>(to_double(a, need(i)));
    else if (a == "--sim-rate") o.sim.rate_hz = static_cast<uint32_t>(to_double(a, need(i)));
    else if (a == "--sim-channels") o.sim.channels = static_cast<uint16_t>(to_double(a, need(i)));
    else if (a == "--sim-garbage") o.sim.garbage_prob = to_double(a, need(i));
    else if (a == "--sim-corrupt") o.sim.corrupt_prob = to_double(a, need(i));
    else if (a == "--sim-unplug-every") o.sim.disconnect_mean_s = to_double(a, need(i));
    else if (a == "--serial") {
      std::string s = need(i);
      SerialSpec spec;
      if (auto at = s.rfind('@'); at != std::string::npos) {
        spec.path = s.substr(0, at);
        spec.baud = static_cast<int>(to_double("--serial", s.substr(at + 1).c_str()));
      } else {
        spec.path = s;
      }
      o.serial.push_back(spec);
    } else if (a == "--tcp") {
      std::string s = need(i);
      auto colon = s.rfind(':');
      if (colon == std::string::npos) throw std::invalid_argument("--tcp wants HOST:PORT");
      o.tcp.push_back({s.substr(0, colon),
                       static_cast<uint16_t>(to_double("--tcp", s.substr(colon + 1).c_str()))});
    } else if (a == "--flush-ms") {
      o.core.reader.flush_interval =
          std::chrono::milliseconds(static_cast<int>(to_double(a, need(i))));
    } else if (a == "--no-batching") {
      o.core.reader.batching = false;
    } else if (a == "--queue") {
      o.core.input_queue_capacity = static_cast<std::size_t>(to_double(a, need(i)));
    } else if (a == "-h" || a == "--help") {
      throw std::invalid_argument("");
    } else {
      throw std::invalid_argument("unknown option " + std::string(a));
    }
  }
  if (o.sim_count == 0 && o.serial.empty() && o.tcp.empty())
    throw std::invalid_argument("no devices: pass --sim N, --serial PATH or --tcp HOST:PORT");
  return o;
}

std::vector<std::unique_ptr<devices::Device>> make_devices(const Options& o) {
  std::vector<std::unique_ptr<devices::Device>> out;
  for (int i = 0; i < o.sim_count; ++i) {
    auto cfg = o.sim;
    cfg.seed = static_cast<uint32_t>(i + 1);
    cfg.heart_rate_bpm = 60.0 + 7.0 * (i % 6);  // so the plots don't all look identical
    out.push_back(std::make_unique<devices::SimulatedDevice>("sim" + std::to_string(i), cfg));
  }
  for (const auto& s : o.serial)
    out.push_back(std::make_unique<devices::SerialDevice>(s.path, s.baud));
  for (const auto& t : o.tcp) out.push_back(std::make_unique<devices::TcpDevice>(t.host, t.port));
  return out;
}

}  // namespace sensord::app
