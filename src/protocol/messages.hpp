#pragma once
// Payloads carried inside frames. First payload byte is the message type.
//
//   Samples  (device -> host): type=1 | channel:u16 | t0_us:u64 | period_us:u32 | count:u16 | f32 * count
//   SetRate  (host -> device): type=2 | rate_hz:u32
//   RateAck  (device -> host): type=3 | rate_hz:u32

#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace sensord::protocol {

enum class MsgType : uint8_t { Samples = 1, SetRate = 2, RateAck = 3 };

struct SamplesMsg {
  uint16_t channel = 0;
  uint64_t t0_us = 0;
  uint32_t period_us = 0;
  std::vector<float> values;
};

struct SetRateMsg {
  uint32_t rate_hz = 0;
};

struct RateAckMsg {
  uint32_t rate_hz = 0;
};

using Message = std::variant<SamplesMsg, SetRateMsg, RateAckMsg>;

inline constexpr std::size_t kSamplesHeader = 1 + 2 + 8 + 4 + 2;
inline constexpr std::size_t kMaxSamplesPerMsg = (1024 - kSamplesHeader) / 4;
inline constexpr uint32_t kMinRateHz = 1;
inline constexpr uint32_t kMaxRateHz = 20000;

void encode_message(std::vector<uint8_t>& out, const Message& msg);
std::vector<uint8_t> encode_message(const Message& msg);

// Returns nullopt for unknown types or payloads that are too short/long.
std::optional<Message> decode_message(std::span<const uint8_t> payload);

}  // namespace sensord::protocol
