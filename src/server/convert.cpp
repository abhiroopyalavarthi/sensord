#include "server/convert.hpp"

namespace sensord::server {

void to_proto(const pipeline::Batch& b, uint64_t subscriber_dropped, sensord::v1::SampleBatch* out) {
  out->set_device_id(b.device_id);
  out->set_connection_epoch(b.connection_epoch);
  out->set_device_frames_lost(b.device_frames_lost);
  out->set_subscriber_dropped(subscriber_dropped);
  out->set_host_rx_mono_ns(b.host_rx_mono_ns);
  out->mutable_blocks()->Reserve(static_cast<int>(b.blocks.size()));
  for (const auto& blk : b.blocks) {
    auto* cs = out->add_blocks();
    cs->set_channel(blk.channel);
    cs->set_frame_seq(blk.frame_seq);
    cs->set_t0_us(blk.t0_us);
    cs->set_period_us(blk.period_us);
    cs->mutable_values()->Add(blk.values.begin(), blk.values.end());
  }
}

void to_proto(const pipeline::DeviceStatus& s, sensord::v1::DeviceInfo* out) {
  out->set_id(s.id);
  out->set_name(s.name);
  out->set_kind(s.kind);
  out->set_connected(s.connected);
  out->set_sample_rate_hz(s.sample_rate_hz);
  out->set_frames(s.frames);
  out->set_crc_errors(s.crc_errors);
  out->set_frames_lost(s.frames_lost);
  out->set_reconnects(s.reconnects);
  out->set_samples(s.samples);
  out->set_connection_epoch(s.connection_epoch);
  out->set_bytes_skipped(s.bytes_skipped);
}

}  // namespace sensord::server
