// sensord: collects sensor data from devices and serves it over gRPC.

#include <csignal>
#include <iostream>

#include "device_spec.hpp"
#include "pipeline/telemetry_core.hpp"
#include "server/telemetry_service.hpp"

int main(int argc, char** argv) {
  using namespace sensord;

  app::Options opts;
  try {
    opts = app::parse_args(argc, argv);
  } catch (const std::invalid_argument& e) {
    if (*e.what()) std::cerr << "sensord: " << e.what() << "\n\n";
    std::cerr << app::usage();
    return 2;
  }

  // Block SIGINT/SIGTERM before any thread starts, so every thread inherits
  // the mask and only main() receives them (via sigwait below). No async
  // signal handler, no races with the worker threads.
  sigset_t sigs;
  sigemptyset(&sigs);
  sigaddset(&sigs, SIGINT);
  sigaddset(&sigs, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &sigs, nullptr);
  std::signal(SIGPIPE, SIG_IGN);

  pipeline::TelemetryCore core(opts.core);
  for (auto& d : app::make_devices(opts)) {
    const auto name = d->name();
    const auto id = core.add_device(std::move(d));
    std::cout << "device " << id << ": " << name << "\n";
  }
  core.start();

  std::unique_ptr<server::Server> srv;
  try {
    srv = std::make_unique<server::Server>(core, opts.listen);
  } catch (const std::exception& e) {
    std::cerr << "sensord: " << e.what() << "\n";
    core.stop();
    return 1;
  }
  std::cout << "sensord listening on " << opts.listen << " (port " << srv->port() << ")"
            << std::endl;

  int sig = 0;
  sigwait(&sigs, &sig);
  std::cout << "\nsensord: got signal " << sig << ", shutting down" << std::endl;

  // Stop the server first so no new RPCs start, then the pipeline.
  srv->stop();
  core.stop();
  std::cout << "sensord: clean exit" << std::endl;
  return 0;
}
