#include "pipeline/device_reader.hpp"

#include <array>
#include <variant>
#include <vector>

#include "protocol/frame.hpp"
#include "protocol/messages.hpp"

namespace sensord::pipeline {

using namespace std::chrono;
namespace proto = sensord::protocol;

namespace {
int64_t mono_ns() {
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}
}  // namespace

DeviceReader::DeviceReader(uint32_t id, std::unique_ptr<devices::Device> dev,
                           BoundedQueue<BatchPtr>& out, ReaderOptions opts)
    : id_(id),
      dev_(std::move(dev)),
      out_(out),
      opts_(opts),
      name_(dev_->name()),
      kind_(dev_->kind()) {}

DeviceReader::~DeviceReader() { stop(); }

void DeviceReader::start() {
  thread_ = std::jthread([this](std::stop_token st) { run(st); });
}

void DeviceReader::stop() {
  if (thread_.joinable()) {
    thread_.request_stop();
    thread_.join();
  }
}

void DeviceReader::request_sample_rate(uint32_t hz) {
  std::lock_guard lk(mu_);
  pending_rate_ = hz;
  desired_rate_ = hz;
}

DeviceStatus DeviceReader::status() const {
  DeviceStatus s;
  s.id = id_;
  s.name = name_;
  s.kind = kind_;
  s.connected = connected_.load();
  s.sample_rate_hz = rate_hz_.load();
  s.frames = frames_.load();
  s.crc_errors = crc_errors_.load();
  s.frames_lost = frames_lost_.load();
  s.reconnects = reconnects_.load();
  s.samples = samples_.load();
  s.bytes_skipped = bytes_skipped_.load();
  s.connection_epoch = epoch_.load();
  std::lock_guard lk(mu_);
  s.last_error = last_error_;
  return s;
}

bool DeviceReader::sleep_backoff(std::stop_token st, milliseconds d) {
  std::unique_lock lk(sleep_mu_);
  // Returns early (true) only if stop was requested.
  return sleep_cv_.wait_for(lk, st, d, [] { return false; }) || st.stop_requested();
}

void DeviceReader::flush(std::stop_token st) {
  if (!batch_ || batch_->blocks.empty()) return;
  batch_->device_frames_lost = frames_lost_.load(std::memory_order_relaxed);
  batch_->host_rx_mono_ns = mono_ns();
  // Blocking push: if the hub falls behind, readers slow down (backpressure)
  // instead of growing memory without bound.
  out_.push(std::move(batch_), st);
  batch_.reset();
}

void DeviceReader::run(std::stop_token st) {
  proto::FrameDecoder decoder;
  std::vector<proto::Frame> frames;
  std::array<uint8_t, 4096> buf{};
  std::vector<uint8_t> cmd;
  std::optional<uint32_t> expected_seq;
  milliseconds backoff = opts_.backoff_initial;
  bool open = false;
  proto::DecoderStats last_stats{};

  while (!st.stop_requested()) {
    if (!open) {
      std::string err;
      if (!dev_->open(err)) {
        {
          std::lock_guard lk(mu_);
          last_error_ = err;
        }
        if (sleep_backoff(st, backoff)) break;
        backoff = std::min(backoff * 2, opts_.backoff_max);
        continue;
      }
      open = true;
      backoff = opts_.backoff_initial;
      decoder.reset();
      last_stats = {};
      expected_seq.reset();
      epoch_.fetch_add(1);
      connected_.store(true);
      {
        std::lock_guard lk(mu_);
        last_error_.clear();
        if (desired_rate_) pending_rate_ = desired_rate_;  // re-apply after reconnect
      }
    }

    // Send any pending command.
    std::optional<uint32_t> rate;
    {
      std::lock_guard lk(mu_);
      rate.swap(pending_rate_);
    }
    if (rate) {
      cmd.clear();
      std::vector<uint8_t> payload;
      proto::encode_message(payload, proto::SetRateMsg{*rate});
      proto::encode_frame_into(cmd, 0, payload);
      if (!dev_->write(cmd)) {
        std::lock_guard lk(mu_);
        pending_rate_ = rate;  // retry after reconnect
      }
    }

    const std::ptrdiff_t n = dev_->read(buf, opts_.read_timeout);
    if (n < 0) {
      flush(st);
      dev_->close();
      open = false;
      connected_.store(false);
      reconnects_.fetch_add(1);
      {
        std::lock_guard lk(mu_);
        last_error_ = "link dropped";
      }
      continue;
    }

    if (n > 0) {
      frames.clear();
      decoder.feed(std::span<const uint8_t>(buf.data(), static_cast<std::size_t>(n)), frames);
      const auto& ds = decoder.stats();
      crc_errors_.fetch_add(ds.crc_errors - last_stats.crc_errors, std::memory_order_relaxed);
      bytes_skipped_.fetch_add(ds.bytes_skipped - last_stats.bytes_skipped,
                               std::memory_order_relaxed);
      frames_.fetch_add(ds.frames - last_stats.frames, std::memory_order_relaxed);
      last_stats = ds;

      for (auto& f : frames) {
        if (expected_seq) {
          const uint32_t gap = f.seq - *expected_seq;  // wraps correctly
          if (gap != 0 && gap < (1u << 31)) frames_lost_.fetch_add(gap, std::memory_order_relaxed);
        }
        expected_seq = f.seq + 1;

        auto msg = proto::decode_message(f.payload);
        if (!msg) continue;
        if (auto* s = std::get_if<proto::SamplesMsg>(&*msg)) {
          if (!batch_) {
            batch_ = std::make_shared<Batch>();
            batch_->device_id = id_;
            batch_->connection_epoch = epoch_.load(std::memory_order_relaxed);
            batch_started_ = steady_clock::now();
          }
          if (s->period_us > 0) rate_hz_.store(1'000'000u / s->period_us, std::memory_order_relaxed);
          samples_.fetch_add(s->values.size(), std::memory_order_relaxed);
          batch_->blocks.push_back(SampleBlock{s->channel, f.seq, s->t0_us, s->period_us,
                                               std::move(s->values)});
          if (!opts_.batching) flush(st);
        } else if (auto* ack = std::get_if<proto::RateAckMsg>(&*msg)) {
          rate_hz_.store(ack->rate_hz, std::memory_order_relaxed);
        }
      }
    }

    if (batch_ && steady_clock::now() - batch_started_ >= opts_.flush_interval) flush(st);
  }

  dev_->close();
  connected_.store(false);
}

}  // namespace sensord::pipeline
