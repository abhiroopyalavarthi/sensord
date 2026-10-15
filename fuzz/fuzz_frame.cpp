// libFuzzer target for the frame decoder + message parser.
//
// The first input byte picks how the rest is chopped into feed() calls, so
// the fuzzer explores split points as well as byte contents. Checks:
//  - nothing crashes / no UB (run under ASan + UBSan)
//  - the decoder never buffers more than one max frame + one chunk
//  - every frame that comes out re-encodes to bytes that decode identically
//  - feeding the same bytes in one go yields the same frames

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "protocol/frame.hpp"
#include "protocol/messages.hpp"

using namespace sensord::protocol;

#define FUZZ_CHECK(cond) \
  do {                   \
    if (!(cond)) std::abort(); \
  } while (0)

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size) {
  if (size == 0) return 0;
  const std::size_t chunk = 1 + data[0] % 64;
  std::span<const uint8_t> in(data + 1, size - 1);

  FrameDecoder split;
  std::vector<Frame> split_frames;
  for (std::size_t off = 0; off < in.size(); off += chunk) {
    auto piece = in.subspan(off, std::min(chunk, in.size() - off));
    split.feed(piece, split_frames);
    FUZZ_CHECK(split.buffered() <= kMaxFrameSize + chunk);
  }

  FrameDecoder whole;
  std::vector<Frame> whole_frames;
  whole.feed(in, whole_frames);

  FUZZ_CHECK(split_frames.size() == whole_frames.size());
  for (std::size_t i = 0; i < whole_frames.size(); ++i) {
    const auto& f = whole_frames[i];
    FUZZ_CHECK(f.seq == split_frames[i].seq);
    FUZZ_CHECK(f.payload == split_frames[i].payload);
    FUZZ_CHECK(f.payload.size() <= kMaxPayload);

    // Round trip.
    auto bytes = encode_frame(f.seq, f.payload);
    FrameDecoder again;
    std::vector<Frame> out;
    again.feed(bytes, out);
    FUZZ_CHECK(out.size() == 1 && out[0].seq == f.seq && out[0].payload == f.payload);

    // The message layer must reject or accept cleanly; accepted messages round-trip.
    if (auto m = decode_message(f.payload)) {
      auto re = encode_message(*m);
      FUZZ_CHECK(re == f.payload);
    }
  }
  return 0;
}
