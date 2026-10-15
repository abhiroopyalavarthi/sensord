#pragma once
// Command-line parsing for sensord.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "devices/device.hpp"
#include "devices/signal_generator.hpp"
#include "pipeline/telemetry_core.hpp"

namespace sensord::app {

struct SerialSpec {
  std::string path;
  int baud = 115200;
};

struct TcpSpec {
  std::string host;
  uint16_t port = 0;
};

struct Options {
  std::string listen = "0.0.0.0:50051";
  int sim_count = 0;
  devices::SimConfig sim;
  std::vector<SerialSpec> serial;
  std::vector<TcpSpec> tcp;
  pipeline::CoreOptions core;
};

// Throws std::invalid_argument with a readable message on bad input.
Options parse_args(int argc, char** argv);
std::string usage();

std::vector<std::unique_ptr<devices::Device>> make_devices(const Options& o);

}  // namespace sensord::app
