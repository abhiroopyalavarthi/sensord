// Writes a handful of seed inputs for the fuzzer into the given directory.
// Byte 0 of each file is the chunk-size selector used by fuzz_frame.cpp.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "protocol/frame.hpp"
#include "protocol/messages.hpp"

using namespace sensord::protocol;

static void write(const std::filesystem::path& dir, const std::string& name, uint8_t chunk,
                  const std::vector<uint8_t>& body) {
  std::ofstream f(dir / name, std::ios::binary);
  f.put(static_cast<char>(chunk));
  f.write(reinterpret_cast<const char*>(body.data()), static_cast<std::streamsize>(body.size()));
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: make_seeds DIR\n");
    return 2;
  }
  std::filesystem::path dir = argv[1];
  std::filesystem::create_directories(dir);

  std::vector<uint8_t> two;
  encode_frame_into(two, 1, encode_message(SamplesMsg{0, 1000, 4000, {0.1f, 0.2f, 0.3f}}));
  encode_frame_into(two, 2, encode_message(RateAckMsg{250}));
  write(dir, "seed_two_frames", 0, two);
  write(dir, "seed_two_frames_split7", 6, two);

  std::vector<uint8_t> junk = {0xA5, 0x00, 0xA5, 0x5A, 0xFF, 0xFF, 0x13};
  junk.insert(junk.end(), two.begin(), two.end());
  write(dir, "seed_junk_then_frames", 2, junk);

  auto bad = two;
  bad[12] ^= 0x40;
  write(dir, "seed_bad_crc", 1, bad);

  std::vector<uint8_t> big;
  SamplesMsg m;
  m.values.assign(kMaxSamplesPerMsg, 1.0f);
  encode_frame_into(big, 9, encode_message(m));
  write(dir, "seed_max_frame", 63, big);

  write(dir, "seed_set_rate", 0, encode_frame(0, encode_message(SetRateMsg{1000})));
  std::printf("wrote seeds to %s\n", dir.c_str());
  return 0;
}
