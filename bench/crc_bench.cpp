// Micro-benchmark: CRC-16 bitwise vs table, over frame-sized buffers.
// Usage: sensord-crc-bench [MB]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "protocol/crc16.hpp"

using namespace sensord::protocol;
using Clock = std::chrono::steady_clock;

template <class F>
double run(F f, const std::vector<uint8_t>& buf, std::size_t frame, std::size_t total, uint16_t& sink) {
  const auto t0 = Clock::now();
  for (std::size_t done = 0; done < total; done += buf.size())
    for (std::size_t off = 0; off + frame <= buf.size(); off += frame)
      sink ^= f(std::span<const uint8_t>(buf.data() + off, frame), 0xFFFF);
  return double(total) / 1e6 / std::chrono::duration<double>(Clock::now() - t0).count();
}

int main(int argc, char** argv) {
  const std::size_t mb = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 256;
  const std::size_t total = mb << 20;
  std::vector<uint8_t> buf(1 << 20);
  uint32_t x = 1;
  for (auto& b : buf) b = static_cast<uint8_t>((x = x * 1664525u + 1013904223u) >> 24);
  const std::size_t frame = 89;  // a 2-channel, 250 Hz frame payload+header is about this size
  uint16_t sink = 0;
  auto bw = [](std::span<const uint8_t> d, uint16_t c) { return crc16_bitwise(d, c); };
  auto tb = [](std::span<const uint8_t> d, uint16_t c) { return crc16(d, c); };
  const double a = run(bw, buf, frame, total, sink);
  const double b = run(tb, buf, frame, total, sink);
  std::printf("crc16 bitwise: %8.1f MB/s\n", a);
  std::printf("crc16 table  : %8.1f MB/s  (%.1fx)\n", b, b / a);
  std::printf("(sink %u)\n", sink);
  return 0;
}
