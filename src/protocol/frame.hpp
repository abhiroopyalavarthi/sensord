#pragma once
// Wire framing between sensord and devices.
//
//   +------+------+--------+--------+-----------+--------+
//   | 0xA5 | 0x5A | len:16 | seq:32 | payload.. | crc:16 |
//   +------+------+--------+--------+-----------+--------+
//
// All integers little-endian. The CRC covers len, seq and payload (not the
// sync bytes). When a CRC check fails the decoder slides forward one byte and
// searches for the next sync pair, so one bad byte costs at most one frame.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sensord::protocol {

inline constexpr uint8_t kSync0 = 0xA5;
inline constexpr uint8_t kSync1 = 0x5A;
inline constexpr std::size_t kHeaderSize = 8;  // sync(2) + len(2) + seq(4)
inline constexpr std::size_t kCrcSize = 2;
inline constexpr std::size_t kMaxPayload = 1024;
inline constexpr std::size_t kMaxFrameSize = kHeaderSize + kMaxPayload + kCrcSize;

struct Frame {
  uint32_t seq = 0;
  std::vector<uint8_t> payload;
};

void encode_frame_into(std::vector<uint8_t>& out, uint32_t seq, std::span<const uint8_t> payload);
std::vector<uint8_t> encode_frame(uint32_t seq, std::span<const uint8_t> payload);

struct DecoderStats {
  uint64_t frames = 0;
  uint64_t crc_errors = 0;
  uint64_t bad_length = 0;
  uint64_t bytes_skipped = 0;  // bytes thrown away while hunting for sync
};

class FrameDecoder {
 public:
  // Feeds raw bytes (any split, any amount). Complete, CRC-valid frames are
  // appended to `out`. Never buffers more than kMaxFrameSize + data.size().
  void feed(std::span<const uint8_t> data, std::vector<Frame>& out);

  void reset();
  const DecoderStats& stats() const { return stats_; }
  std::size_t buffered() const { return buf_.size(); }

 private:
  std::vector<uint8_t> buf_;
  DecoderStats stats_;
};

}  // namespace sensord::protocol
