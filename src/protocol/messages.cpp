#include "protocol/messages.hpp"

#include <stdexcept>

#include "protocol/bytes.hpp"

namespace sensord::protocol {

namespace {
template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;
}  // namespace

void encode_message(std::vector<uint8_t>& out, const Message& msg) {
  std::visit(overloaded{
                 [&](const SamplesMsg& m) {
                   if (m.values.size() > kMaxSamplesPerMsg)
                     throw std::length_error("too many samples in one message");
                   out.push_back(static_cast<uint8_t>(MsgType::Samples));
                   put_u16(out, m.channel);
                   put_u64(out, m.t0_us);
                   put_u32(out, m.period_us);
                   put_u16(out, static_cast<uint16_t>(m.values.size()));
                   for (float v : m.values) put_f32(out, v);
                 },
                 [&](const SetRateMsg& m) {
                   out.push_back(static_cast<uint8_t>(MsgType::SetRate));
                   put_u32(out, m.rate_hz);
                 },
                 [&](const RateAckMsg& m) {
                   out.push_back(static_cast<uint8_t>(MsgType::RateAck));
                   put_u32(out, m.rate_hz);
                 },
             },
             msg);
}

std::vector<uint8_t> encode_message(const Message& msg) {
  std::vector<uint8_t> out;
  encode_message(out, msg);
  return out;
}

std::optional<Message> decode_message(std::span<const uint8_t> p) {
  if (p.empty()) return std::nullopt;
  switch (static_cast<MsgType>(p[0])) {
    case MsgType::Samples: {
      if (p.size() < kSamplesHeader) return std::nullopt;
      SamplesMsg m;
      m.channel = get_u16(&p[1]);
      m.t0_us = get_u64(&p[3]);
      m.period_us = get_u32(&p[11]);
      const uint16_t count = get_u16(&p[15]);
      if (p.size() != kSamplesHeader + static_cast<std::size_t>(count) * 4) return std::nullopt;
      m.values.resize(count);
      for (uint16_t i = 0; i < count; ++i) m.values[i] = get_f32(&p[kSamplesHeader + 4u * i]);
      return m;
    }
    case MsgType::SetRate:
      if (p.size() != 5) return std::nullopt;
      return SetRateMsg{get_u32(&p[1])};
    case MsgType::RateAck:
      if (p.size() != 5) return std::nullopt;
      return RateAckMsg{get_u32(&p[1])};
  }
  return std::nullopt;
}

}  // namespace sensord::protocol
