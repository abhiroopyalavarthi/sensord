#include <gtest/gtest.h>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#include <thread>

#include "devices/serial_device.hpp"
#include "devices/simulated_device.hpp"
#include "devices/tcp_device.hpp"
#include "protocol/frame.hpp"
#include "protocol/messages.hpp"
#include "test_util.hpp"

using namespace sensord;
using namespace std::chrono_literals;
namespace proto = sensord::protocol;

namespace {

// Reads from a device for `dur`, decoding everything.
std::vector<proto::Frame> read_frames(devices::Device& dev, std::chrono::milliseconds dur,
                                      proto::FrameDecoder& dec, bool* dropped = nullptr) {
  std::vector<proto::Frame> frames;
  std::array<uint8_t, 2048> buf{};
  auto end = std::chrono::steady_clock::now() + dur;
  while (std::chrono::steady_clock::now() < end) {
    auto n = dev.read(buf, 10ms);
    if (n < 0) {
      if (dropped) *dropped = true;
      break;
    }
    if (n > 0) dec.feed(std::span<const uint8_t>(buf.data(), static_cast<std::size_t>(n)), frames);
  }
  return frames;
}

std::vector<uint8_t> set_rate_frame(uint32_t hz) {
  return proto::encode_frame(0, proto::encode_message(proto::SetRateMsg{hz}));
}

}  // namespace

TEST(SimulatedDevice, ProducesValidContiguousFrames) {
  devices::SimConfig cfg;
  cfg.channels = 2;
  cfg.rate_hz = 500;
  devices::SimulatedDevice dev("sim", cfg);
  std::string err;
  ASSERT_TRUE(dev.open(err)) << err;
  proto::FrameDecoder dec;
  auto frames = read_frames(dev, 200ms, dec);
  ASSERT_GT(frames.size(), 10u);
  std::size_t samples = 0;
  for (std::size_t i = 0; i < frames.size(); ++i) {
    if (i) EXPECT_EQ(frames[i].seq, frames[i - 1].seq + 1);
    auto m = proto::decode_message(frames[i].payload);
    ASSERT_TRUE(m);
    auto& s = std::get<proto::SamplesMsg>(*m);
    EXPECT_LT(s.channel, 2);
    EXPECT_EQ(s.period_us, 2000u);
    samples += s.values.size();
  }
  // ~200 ms * 500 Hz * 2 channels = ~200 samples; allow scheduling slack.
  EXPECT_GT(samples, 100u);
  EXPECT_LT(samples, 400u);
  EXPECT_EQ(dec.stats().crc_errors, 0u);
}

TEST(SimulatedDevice, AnswersSetRate) {
  devices::SimulatedDevice dev("sim", {});
  std::string err;
  ASSERT_TRUE(dev.open(err));
  ASSERT_TRUE(dev.write(set_rate_frame(1000)));
  proto::FrameDecoder dec;
  auto frames = read_frames(dev, 100ms, dec);
  bool acked = false, fast = false;
  for (auto& f : frames) {
    auto m = proto::decode_message(f.payload);
    ASSERT_TRUE(m);
    if (auto* a = std::get_if<proto::RateAckMsg>(&*m)) acked = a->rate_hz == 1000;
    if (auto* s = std::get_if<proto::SamplesMsg>(&*m)) fast |= s->period_us == 1000;
  }
  EXPECT_TRUE(acked);
  EXPECT_TRUE(fast);
}

TEST(SimulatedDevice, GarbageAndCorruptionAreCaughtByDecoder) {
  devices::SimConfig cfg;
  cfg.rate_hz = 1000;
  cfg.garbage_prob = 0.3;
  cfg.corrupt_prob = 0.1;
  devices::SimulatedDevice dev("sim", cfg);
  std::string err;
  ASSERT_TRUE(dev.open(err));
  proto::FrameDecoder dec;
  auto frames = read_frames(dev, 300ms, dec);
  EXPECT_GT(frames.size(), 20u);
  EXPECT_GT(dec.stats().bytes_skipped, 0u);
  for (auto& f : frames) EXPECT_TRUE(proto::decode_message(f.payload)) << "seq " << f.seq;
}

TEST(SimulatedDevice, UnplugsAndComesBack) {
  devices::SimConfig cfg;
  cfg.disconnect_mean_s = 0.05;
  cfg.reconnect_delay_s = 0.15;
  devices::SimulatedDevice dev("sim", cfg);
  std::string err;
  ASSERT_TRUE(dev.open(err));
  proto::FrameDecoder dec;
  bool dropped = false;
  read_frames(dev, 2000ms, dec, &dropped);
  ASSERT_TRUE(dropped);
  dev.close();
  EXPECT_FALSE(dev.open(err)) << "should still be unplugged";
  std::this_thread::sleep_for(200ms);
  EXPECT_TRUE(dev.open(err)) << err;
}

// The serial code is exercised with no hardware: a pseudo-terminal pair stands
// in for the USB cable. We write device bytes into the master side and
// SerialDevice reads them from the slave path like it would /dev/cu.usbserial.
TEST(SerialDevice, ReadsFramesThroughPseudoTerminal) {
  int master = -1, slave = -1;
  char name[256] = {};
  ASSERT_EQ(::openpty(&master, &slave, name, nullptr, nullptr), 0);
  termios tio{};
  ::tcgetattr(slave, &tio);
  ::cfmakeraw(&tio);
  ::tcsetattr(slave, TCSANOW, &tio);

  devices::SerialDevice dev(name, 115200);
  std::string err;
  ASSERT_TRUE(dev.open(err)) << err;

  std::vector<uint8_t> bytes;
  for (uint32_t s = 0; s < 20; ++s)
    proto::encode_frame_into(bytes, s, proto::encode_message(proto::SamplesMsg{0, s, 1000, {1.f, 2.f}}));
  ASSERT_EQ(::write(master, bytes.data(), bytes.size()), static_cast<ssize_t>(bytes.size()));

  proto::FrameDecoder dec;
  auto frames = read_frames(dev, 200ms, dec);
  ASSERT_EQ(frames.size(), 20u);
  EXPECT_EQ(frames.back().seq, 19u);

  // Command path: host -> device.
  auto cmd = set_rate_frame(200);
  ASSERT_TRUE(dev.write(cmd));
  std::vector<uint8_t> got(cmd.size());
  std::size_t have = 0;
  for (int i = 0; i < 50 && have < got.size(); ++i) {
    auto n = devices::poll_read(master, std::span(got).subspan(have), 20ms);
    if (n > 0) have += static_cast<std::size_t>(n);
  }
  EXPECT_EQ(got, cmd);

  // Unplug: the other end disappears, read must report the drop.
  ::close(master);
  ::close(slave);
  bool dropped = false;
  read_frames(dev, 500ms, dec, &dropped);
  EXPECT_TRUE(dropped);
  dev.close();
}

TEST(SerialDevice, OpenFailsForMissingPort) {
  devices::SerialDevice dev("/dev/does-not-exist-sensord");
  std::string err;
  EXPECT_FALSE(dev.open(err));
  EXPECT_NE(err.find("does-not-exist"), std::string::npos);
}

TEST(TcpDevice, StreamsFromSimServerAndDetectsClose) {
  devices::SimConfig cfg;
  cfg.rate_hz = 1000;
  test::TcpSimServer srv(cfg);
  devices::TcpDevice dev("127.0.0.1", srv.port());
  std::string err;
  ASSERT_TRUE(dev.open(err)) << err;
  proto::FrameDecoder dec;
  auto frames = read_frames(dev, 150ms, dec);
  EXPECT_GT(frames.size(), 5u);
  srv.kick();
  bool dropped = false;
  read_frames(dev, 500ms, dec, &dropped);
  EXPECT_TRUE(dropped);
}

TEST(TcpDevice, ConnectRefused) {
  // Grab a free port, then close it so nothing listens there.
  uint16_t port;
  {
    test::TcpSimServer tmp({});
    port = tmp.port();
  }
  devices::TcpDevice dev("127.0.0.1", port);
  std::string err;
  EXPECT_FALSE(dev.open(err));
  EXPECT_FALSE(err.empty());
}
