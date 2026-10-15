#pragma once

#include <cstdint>
#include <span>

namespace sensord::protocol {

// CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no xorout.
// Check value for "123456789" is 0x29B1.
//
// Table-driven (one lookup per byte). The first version did 8 shift/xor steps
// per byte; perf showed it at 18% of all CPU samples under load because every
// frame is CRC'd twice (device encode + host decode). See README "Profiling".
uint16_t crc16(std::span<const uint8_t> data, uint16_t crc = 0xFFFF);

// Original bit-at-a-time version, kept as the reference implementation for
// tests and for the before/after micro-benchmark.
uint16_t crc16_bitwise(std::span<const uint8_t> data, uint16_t crc = 0xFFFF);

}  // namespace sensord::protocol
