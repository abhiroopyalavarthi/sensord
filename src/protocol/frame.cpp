#include "protocol/frame.hpp"

#include <algorithm>
#include <stdexcept>

#include "protocol/bytes.hpp"
#include "protocol/crc16.hpp"

namespace sensord::protocol {

void encode_frame_into(std::vector<uint8_t>& out, uint32_t seq, std::span<const uint8_t> payload) {
  if (payload.size() > kMaxPayload) throw std::length_error("frame payload too large");
  const std::size_t start = out.size();
  out.reserve(start + kHeaderSize + payload.size() + kCrcSize);
  out.push_back(kSync0);
  out.push_back(kSync1);
  put_u16(out, static_cast<uint16_t>(payload.size()));
  put_u32(out, seq);
  out.insert(out.end(), payload.begin(), payload.end());
  const uint16_t crc = crc16(std::span<const uint8_t>(out).subspan(start + 2));
  put_u16(out, crc);
}

std::vector<uint8_t> encode_frame(uint32_t seq, std::span<const uint8_t> payload) {
  std::vector<uint8_t> out;
  encode_frame_into(out, seq, payload);
  return out;
}

void FrameDecoder::reset() {
  buf_.clear();
  stats_ = {};
}

void FrameDecoder::feed(std::span<const uint8_t> data, std::vector<Frame>& out) {
  buf_.insert(buf_.end(), data.begin(), data.end());

  std::size_t pos = 0;
  const std::size_t n = buf_.size();

  while (true) {
    // Hunt for the sync pair.
    std::size_t sync = pos;
    while (sync + 1 < n && !(buf_[sync] == kSync0 && buf_[sync + 1] == kSync1)) ++sync;
    if (sync + 1 >= n) {
      // No full sync pair. Keep a trailing 0xA5, it might be half of one.
      const std::size_t keep_from = (n > pos && buf_[n - 1] == kSync0) ? n - 1 : n;
      stats_.bytes_skipped += keep_from - pos;
      pos = keep_from;
      break;
    }
    stats_.bytes_skipped += sync - pos;
    pos = sync;

    if (n - pos < kHeaderSize) break;  // wait for the rest of the header

    const uint16_t len = get_u16(&buf_[pos + 2]);
    if (len > kMaxPayload) {
      ++stats_.bad_length;
      ++stats_.bytes_skipped;
      pos += 1;  // this was not a real sync, keep looking
      continue;
    }

    const std::size_t total = kHeaderSize + len + kCrcSize;
    if (n - pos < total) break;  // wait for the rest of the frame

    const uint16_t want = get_u16(&buf_[pos + kHeaderSize + len]);
    const uint16_t got =
        crc16(std::span<const uint8_t>(&buf_[pos + 2], kHeaderSize - 2 + len));
    if (want != got) {
      ++stats_.crc_errors;
      ++stats_.bytes_skipped;
      pos += 1;  // resync: scan forward from the next byte
      continue;
    }

    Frame f;
    f.seq = get_u32(&buf_[pos + 4]);
    f.payload.assign(buf_.begin() + static_cast<std::ptrdiff_t>(pos + kHeaderSize),
                     buf_.begin() + static_cast<std::ptrdiff_t>(pos + kHeaderSize + len));
    out.push_back(std::move(f));
    ++stats_.frames;
    pos += total;
  }

  buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos));
}

}  // namespace sensord::protocol
