#include <gtest/gtest.h>

#include <string_view>

#include "protocol/crc16.hpp"

using sensord::protocol::crc16;

static std::span<const uint8_t> bytes(std::string_view s) {
  return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

TEST(Crc16, StandardCheckValue) {
  // CRC-16/CCITT-FALSE check value from the CRC catalogue.
  EXPECT_EQ(crc16(bytes("123456789")), 0x29B1);
}

TEST(Crc16, EmptyIsInit) { EXPECT_EQ(crc16({}), 0xFFFF); }

TEST(Crc16, TableMatchesBitwiseReference) {
  using sensord::protocol::crc16_bitwise;
  EXPECT_EQ(crc16_bitwise(bytes("123456789")), 0x29B1);
  std::vector<uint8_t> d;
  uint32_t x = 12345;
  for (int len = 0; len < 2000; ++len) {
    EXPECT_EQ(crc16(d), crc16_bitwise(d)) << "len " << len;
    x = x * 1664525u + 1013904223u;
    d.push_back(static_cast<uint8_t>(x >> 24));
  }
}

TEST(Crc16, IncrementalMatchesOneShot) {
  auto all = bytes("hello, sensor world");
  uint16_t c = crc16(all.subspan(0, 5));
  c = crc16(all.subspan(5), c);
  EXPECT_EQ(c, crc16(all));
}

TEST(Crc16, SingleBitFlipChangesCrc) {
  std::vector<uint8_t> d(64, 0x42);
  const auto good = crc16(d);
  for (std::size_t i = 0; i < d.size(); ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      d[i] ^= static_cast<uint8_t>(1 << bit);
      EXPECT_NE(crc16(d), good) << "byte " << i << " bit " << bit;
      d[i] ^= static_cast<uint8_t>(1 << bit);
    }
  }
}
