#pragma once
// Little-endian helpers for the wire format.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace sensord::protocol {

inline void put_u16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>(v >> 8));
}

inline void put_u32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

inline void put_u64(std::vector<uint8_t>& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

inline void put_f32(std::vector<uint8_t>& out, float f) {
  uint32_t v;
  std::memcpy(&v, &f, sizeof v);
  put_u32(out, v);
}

inline uint16_t get_u16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline uint32_t get_u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t get_u64(const uint8_t* p) {
  return static_cast<uint64_t>(get_u32(p)) | (static_cast<uint64_t>(get_u32(p + 4)) << 32);
}

inline float get_f32(const uint8_t* p) {
  uint32_t v = get_u32(p);
  float f;
  std::memcpy(&f, &v, sizeof f);
  return f;
}

}  // namespace sensord::protocol
