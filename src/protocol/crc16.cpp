#include "protocol/crc16.hpp"

#include <array>

namespace sensord::protocol {

namespace {
constexpr std::array<uint16_t, 256> make_table() {
  std::array<uint16_t, 256> t{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint16_t c = static_cast<uint16_t>(i << 8);
    for (int bit = 0; bit < 8; ++bit)
      c = static_cast<uint16_t>((c & 0x8000) ? (c << 1) ^ 0x1021 : (c << 1));
    t[i] = c;
  }
  return t;
}
constexpr auto kTable = make_table();
static_assert(kTable[1] == 0x1021);
}  // namespace

uint16_t crc16(std::span<const uint8_t> data, uint16_t crc) {
  for (uint8_t byte : data)
    crc = static_cast<uint16_t>((crc << 8) ^ kTable[((crc >> 8) ^ byte) & 0xFF]);
  return crc;
}

uint16_t crc16_bitwise(std::span<const uint8_t> data, uint16_t crc) {
  for (uint8_t byte : data) {
    crc = static_cast<uint16_t>(crc ^ (static_cast<uint16_t>(byte) << 8));
    for (int bit = 0; bit < 8; ++bit) {
      if (crc & 0x8000)
        crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
      else
        crc = static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

}  // namespace sensord::protocol
