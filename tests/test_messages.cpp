#include <gtest/gtest.h>

#include "protocol/messages.hpp"

using namespace sensord::protocol;

TEST(Messages, SamplesRoundTrip) {
  SamplesMsg m{3, 123456789012ull, 4000, {1.5f, -2.25f, 0.0f, 1e6f}};
  auto bytes = encode_message(m);
  EXPECT_EQ(bytes.size(), kSamplesHeader + 4 * 4);
  auto back = decode_message(bytes);
  ASSERT_TRUE(back);
  auto* s = std::get_if<SamplesMsg>(&*back);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->channel, 3);
  EXPECT_EQ(s->t0_us, 123456789012ull);
  EXPECT_EQ(s->period_us, 4000u);
  EXPECT_EQ(s->values, m.values);
}

TEST(Messages, SetRateAndAck) {
  auto a = decode_message(encode_message(SetRateMsg{500}));
  ASSERT_TRUE(a);
  EXPECT_EQ(std::get<SetRateMsg>(*a).rate_hz, 500u);
  auto b = decode_message(encode_message(RateAckMsg{1000}));
  ASSERT_TRUE(b);
  EXPECT_EQ(std::get<RateAckMsg>(*b).rate_hz, 1000u);
}

TEST(Messages, MaxSamplesFitsInOneFrame) {
  SamplesMsg m;
  m.values.resize(kMaxSamplesPerMsg);
  EXPECT_LE(encode_message(m).size(), 1024u);
  m.values.push_back(0);
  EXPECT_THROW(encode_message(m), std::length_error);
}

TEST(Messages, RejectsMalformed) {
  EXPECT_FALSE(decode_message({}));
  std::vector<uint8_t> unknown = {0x77, 1, 2, 3};
  EXPECT_FALSE(decode_message(unknown));

  auto good = encode_message(SamplesMsg{0, 0, 1000, {1, 2, 3}});
  for (std::size_t n = 0; n < good.size(); ++n)
    EXPECT_FALSE(decode_message(std::span(good).subspan(0, n))) << "truncated to " << n;
  good.push_back(0);
  EXPECT_FALSE(decode_message(good)) << "trailing byte";

  std::vector<uint8_t> short_rate = {2, 1, 2};
  EXPECT_FALSE(decode_message(short_rate));
}
