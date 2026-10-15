#pragma once
// Test helper: a tiny TCP "sensor board" on 127.0.0.1 that streams
// SignalGenerator output. Can drop the current client to simulate an unplug.

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "devices/fd_util.hpp"
#include "devices/signal_generator.hpp"

namespace sensord::test {

class TcpSimServer {
 public:
  explicit TcpSimServer(devices::SimConfig cfg) : cfg_(cfg) {
    ls_ = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    ::setsockopt(ls_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    ::bind(ls_, reinterpret_cast<sockaddr*>(&a), sizeof a);
    ::listen(ls_, 4);
    socklen_t len = sizeof a;
    ::getsockname(ls_, reinterpret_cast<sockaddr*>(&a), &len);
    port_ = ntohs(a.sin_port);
    thread_ = std::jthread([this](std::stop_token st) { run(st); });
  }

  ~TcpSimServer() {
    thread_.request_stop();
    thread_.join();
    ::close(ls_);
  }

  uint16_t port() const { return port_; }
  int accepted() const { return accepted_.load(); }
  // Closes the current client connection (the device "unplugs").
  void kick() { kick_.store(true); }

 private:
  void run(std::stop_token st) {
    devices::SignalGenerator gen(cfg_);
    std::vector<uint8_t> out, in(512);
    while (!st.stop_requested()) {
      pollfd p{ls_, POLLIN, 0};
      if (::poll(&p, 1, 20) <= 0) continue;
      int c = ::accept(ls_, nullptr, nullptr);
      if (c < 0) continue;
      devices::set_nonblocking(c);
      accepted_.fetch_add(1);
      while (!st.stop_requested() && !kick_.exchange(false)) {
        auto n = devices::poll_read(c, in, std::chrono::milliseconds(2));
        if (n < 0) break;
        out.clear();
        if (n > 0) gen.on_host_bytes(std::span(in.data(), static_cast<std::size_t>(n)), out);
        gen.generate(std::chrono::steady_clock::now(), out);
        if (!out.empty() && !devices::write_all(c, out, true)) break;
      }
      ::close(c);
    }
  }

  devices::SimConfig cfg_;
  int ls_ = -1;
  uint16_t port_ = 0;
  std::atomic<int> accepted_{0};
  std::atomic<bool> kick_{false};
  std::jthread thread_;
};

}  // namespace sensord::test
