// sensord-sim: a fake sensor board you can plug in "for real".
//
//   sensord-sim --tcp 9000      listen on TCP port 9000 (one client at a time)
//   sensord-sim --pty           create a pseudo-terminal and print its path;
//                               point `sensord --serial <path>` at it to test
//                               the serial code with no hardware.
//
// Kill it (Ctrl-C) to "unplug" the device, start it again to "plug it back in".

#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "devices/fd_util.hpp"
#include "devices/signal_generator.hpp"

using namespace sensord::devices;
using Clock = std::chrono::steady_clock;

namespace {
std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

void usage() {
  std::cerr << "usage: sensord-sim (--tcp PORT | --pty) [--rate HZ] [--channels N]\n"
               "                   [--garbage P] [--corrupt P] [--seed N] [--hr BPM]\n";
}

// Streams generator output to `out_fd`, reads commands from `in_fd`.
// Returns when the peer goes away or we are asked to stop.
// `drop_when_full`: for a pty nobody is reading, drop bytes like a real UART would.
void serve(SignalGenerator& gen, int fd, bool drop_when_full, bool use_send) {
  std::vector<uint8_t> out;
  std::vector<uint8_t> in(1024);
  while (!g_stop.load()) {
    auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(gen.next_due() - Clock::now());
    if (wait.count() < 0) wait = std::chrono::milliseconds(0);
    const auto n = poll_read(fd, in, wait);
    if (n < 0) return;  // host closed the link
    out.clear();
    if (n > 0) gen.on_host_bytes(std::span(in.data(), static_cast<std::size_t>(n)), out);
    gen.generate(Clock::now(), out);
    if (out.empty()) continue;
    if (drop_when_full) {
      ssize_t w = ::write(fd, out.data(), out.size());
      if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return;
    } else if (!write_all(fd, out, use_send)) {
      return;
    }
  }
}
}  // namespace

int main(int argc, char** argv) {
  SimConfig cfg;
  int tcp_port = -1;
  bool pty = false;
  for (int i = 1; i < argc; ++i) {
    std::string_view a = argv[i];
    auto val = [&]() -> std::string {
      if (i + 1 >= argc) {
        usage();
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--tcp") tcp_port = std::stoi(val());
    else if (a == "--pty") pty = true;
    else if (a == "--rate") cfg.rate_hz = static_cast<uint32_t>(std::stoul(val()));
    else if (a == "--channels") cfg.channels = static_cast<uint16_t>(std::stoul(val()));
    else if (a == "--garbage") cfg.garbage_prob = std::stod(val());
    else if (a == "--corrupt") cfg.corrupt_prob = std::stod(val());
    else if (a == "--seed") cfg.seed = static_cast<uint32_t>(std::stoul(val()));
    else if (a == "--hr") cfg.heart_rate_bpm = std::stod(val());
    else {
      usage();
      return 2;
    }
  }
  if ((tcp_port < 0) == !pty) {
    usage();
    return 2;
  }

  struct sigaction sa{};
  sa.sa_handler = on_signal;  // no SA_RESTART: poll()/accept() return EINTR
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  std::signal(SIGPIPE, SIG_IGN);

  SignalGenerator gen(cfg);

  if (pty) {
    int master = -1, slave = -1;
    char name[256] = {};
    if (::openpty(&master, &slave, name, nullptr, nullptr) != 0) {
      std::perror("openpty");
      return 1;
    }
    termios tio{};
    ::tcgetattr(slave, &tio);
    ::cfmakeraw(&tio);
    ::tcsetattr(slave, TCSANOW, &tio);
    set_nonblocking(master);
    // Keep `slave` open ourselves so the master doesn't see a hangup while
    // sensord reconnects.
    std::cout << name << std::endl;
    std::cerr << "sensord-sim: serving on pty " << name << " (" << cfg.rate_hz << " Hz, "
              << cfg.channels << " ch)\n";
    serve(gen, master, /*drop_when_full=*/true, /*use_send=*/false);
    ::close(master);
    ::close(slave);
    return 0;
  }

  int ls = ::socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  ::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(tcp_port));
  if (::bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(ls, 1) != 0) {
    std::perror("bind/listen");
    return 1;
  }
  std::cerr << "sensord-sim: listening on tcp port " << tcp_port << " (" << cfg.rate_hz
            << " Hz, " << cfg.channels << " ch)\n";

  while (!g_stop.load()) {
    pollfd p{ls, POLLIN, 0};
    if (::poll(&p, 1, 200) <= 0) continue;
    int c = ::accept(ls, nullptr, nullptr);
    if (c < 0) continue;
#ifdef SO_NOSIGPIPE
    ::setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    set_nonblocking(c);
    std::cerr << "sensord-sim: host connected\n";
    serve(gen, c, /*drop_when_full=*/false, /*use_send=*/true);
    if (g_stop.load()) {
      // Close the listener BEFORE the client. The other order leaves a short
      // window where sensord sees EOF, reconnects instantly into the still-
      // open listen backlog, and then gets reset, so one unplug was counted
      // as two reconnects. Found by watching the RECON column; confirmed
      // with strace (see docs/bug-log.md).
      ::close(ls);
      ls = -1;
    }
    ::close(c);
    std::cerr << "sensord-sim: host disconnected\n";
  }
  if (ls >= 0) ::close(ls);
  return 0;
}
