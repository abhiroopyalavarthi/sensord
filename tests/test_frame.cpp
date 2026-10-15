#include <gtest/gtest.h>

#include <random>

#include "protocol/frame.hpp"

using namespace sensord::protocol;

namespace {
std::vector<uint8_t> payload_of(std::size_t n, uint8_t seed = 1) {
  std::vector<uint8_t> p(n);
  for (std::size_t i = 0; i < n; ++i) p[i] = static_cast<uint8_t>(seed + i * 7);
  return p;
}

std::vector<Frame> decode_all(const std::vector<uint8_t>& bytes, FrameDecoder& d) {
  std::vector<Frame> out;
  d.feed(bytes, out);
  return out;
}
}  // namespace

TEST(Frame, RoundTrip) {
  auto p = payload_of(40);
  auto bytes = encode_frame(1234, p);
  ASSERT_EQ(bytes.size(), kHeaderSize + p.size() + kCrcSize);
  FrameDecoder d;
  auto frames = decode_all(bytes, d);
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0].seq, 1234u);
  EXPECT_EQ(frames[0].payload, p);
  EXPECT_EQ(d.buffered(), 0u);
}

TEST(Frame, EmptyAndMaxPayload) {
  FrameDecoder d;
  std::vector<uint8_t> bytes = encode_frame(1, {});
  auto big = payload_of(kMaxPayload);
  encode_frame_into(bytes, 2, big);
  auto frames = decode_all(bytes, d);
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_TRUE(frames[0].payload.empty());
  EXPECT_EQ(frames[1].payload, big);
}

TEST(Frame, EncodeRejectsOversizePayload) {
  auto too_big = payload_of(kMaxPayload + 1);
  EXPECT_THROW(encode_frame(0, too_big), std::length_error);
}

TEST(Frame, SplitOneByteAtATime) {
  auto p = payload_of(100);
  auto bytes = encode_frame(7, p);
  FrameDecoder d;
  std::vector<Frame> out;
  for (uint8_t b : bytes) {
    EXPECT_TRUE(out.empty());
    d.feed(std::span<const uint8_t>(&b, 1), out);
  }
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].payload, p);
}

TEST(Frame, SplitAtEveryPosition) {
  std::vector<uint8_t> bytes = encode_frame(1, payload_of(30, 3));
  encode_frame_into(bytes, 2, payload_of(17, 9));
  for (std::size_t cut = 0; cut <= bytes.size(); ++cut) {
    FrameDecoder d;
    std::vector<Frame> out;
    d.feed(std::span(bytes).subspan(0, cut), out);
    d.feed(std::span(bytes).subspan(cut), out);
    ASSERT_EQ(out.size(), 2u) << "cut at " << cut;
    EXPECT_EQ(out[0].seq, 1u);
    EXPECT_EQ(out[1].seq, 2u);
  }
}

TEST(Frame, BackToBackFrames) {
  std::vector<uint8_t> bytes;
  for (uint32_t s = 0; s < 50; ++s) encode_frame_into(bytes, s, payload_of(s, static_cast<uint8_t>(s)));
  FrameDecoder d;
  auto frames = decode_all(bytes, d);
  ASSERT_EQ(frames.size(), 50u);
  for (uint32_t s = 0; s < 50; ++s) {
    EXPECT_EQ(frames[s].seq, s);
    EXPECT_EQ(frames[s].payload.size(), s);
  }
  EXPECT_EQ(d.stats().crc_errors, 0u);
  EXPECT_EQ(d.stats().bytes_skipped, 0u);
}

TEST(Frame, BadCrcFrameIsDroppedNextFrameSurvives) {
  std::vector<uint8_t> bytes = encode_frame(1, payload_of(20));
  bytes[kHeaderSize + 5] ^= 0x10;  // corrupt payload of frame 1
  encode_frame_into(bytes, 2, payload_of(20, 5));
  FrameDecoder d;
  auto frames = decode_all(bytes, d);
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0].seq, 2u);
  EXPECT_EQ(d.stats().crc_errors, 1u);
}

TEST(Frame, CorruptedLengthFieldResyncs) {
  std::vector<uint8_t> bytes = encode_frame(1, payload_of(20));
  bytes[2] = 0xFF;  // length becomes huge (> kMaxPayload)
  bytes[3] = 0xFF;
  encode_frame_into(bytes, 2, payload_of(20));
  FrameDecoder d;
  auto frames = decode_all(bytes, d);
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0].seq, 2u);
  EXPECT_GE(d.stats().bad_length, 1u);
}

TEST(Frame, CorruptedLengthThatStillFitsResyncs) {
  // Length changed to a smaller valid value: CRC fails, decoder must still
  // find the next frame (which starts inside what it thought was the frame).
  std::vector<uint8_t> bytes = encode_frame(1, payload_of(50));
  bytes[2] = 10;
  bytes[3] = 0;
  encode_frame_into(bytes, 2, payload_of(8));
  FrameDecoder d;
  auto frames = decode_all(bytes, d);
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0].seq, 2u);
}

TEST(Frame, GarbageBetweenFramesIncludingFakeSync) {
  std::vector<uint8_t> bytes = {0x00, 0xA5, 0x13, 0xA5, 0x5A, 0x01};  // junk with a fake sync
  encode_frame_into(bytes, 10, payload_of(12));
  bytes.insert(bytes.end(), {0xA5, 0xA5, 0x5A, 0xFF, 0xFF, 0x00});
  encode_frame_into(bytes, 11, payload_of(12));
  FrameDecoder d;
  auto frames = decode_all(bytes, d);
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_EQ(frames[0].seq, 10u);
  EXPECT_EQ(frames[1].seq, 11u);
  EXPECT_GT(d.stats().bytes_skipped, 0u);
}

TEST(Frame, TrailingHalfSyncIsKept) {
  auto frame = encode_frame(3, payload_of(4));
  std::vector<uint8_t> first = {0x11, 0x22, kSync0};
  std::vector<uint8_t> rest(frame.begin() + 1, frame.end());
  FrameDecoder d;
  std::vector<Frame> out;
  d.feed(first, out);
  EXPECT_EQ(d.buffered(), 1u);
  d.feed(rest, out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].seq, 3u);
}

TEST(Frame, PureGarbageDoesNotGrowBuffer) {
  std::mt19937 rng(42);
  FrameDecoder d;
  std::vector<Frame> out;
  std::vector<uint8_t> junk(4096);
  for (int round = 0; round < 50; ++round) {
    for (auto& b : junk) b = static_cast<uint8_t>(rng());
    d.feed(junk, out);
    EXPECT_LE(d.buffered(), kMaxFrameSize);
  }
}

TEST(Frame, RandomCorruptionNeverYieldsBadFrames) {
  // Corrupt a stream at random and check every frame that comes out is one
  // we actually sent (CRC-16 can in theory let a corruption through, but with
  // single-byte flips it cannot).
  std::mt19937 rng(7);
  std::vector<uint8_t> bytes;
  for (uint32_t s = 0; s < 500; ++s) encode_frame_into(bytes, s, payload_of(1 + s % 60, static_cast<uint8_t>(s)));
  std::uniform_int_distribution<std::size_t> pos(0, bytes.size() - 1);
  for (int i = 0; i < 100; ++i) bytes[pos(rng)] ^= static_cast<uint8_t>(1u << (rng() % 8));
  FrameDecoder d;
  auto frames = decode_all(bytes, d);
  EXPECT_GT(frames.size(), 300u);
  EXPECT_GT(d.stats().crc_errors, 0u);
  uint32_t last = 0;
  bool first = true;
  for (const auto& f : frames) {
    EXPECT_EQ(f.payload, payload_of(1 + f.seq % 60, static_cast<uint8_t>(f.seq)));
    if (!first) EXPECT_GT(f.seq, last);
    last = f.seq;
    first = false;
  }
}
