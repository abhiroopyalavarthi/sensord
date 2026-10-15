// ESP32 firmware for sensord (Arduino core for ESP32).
//
// Streams real ADC readings over USB serial using the sensord wire format,
// and obeys SetRate commands from the host. Wire a sensor (a PPG module like
// the MAX30102 analog breakout, a photoresistor divider, or a potentiometer)
// to the ADC pins below.
//
//   Build/flash: Arduino IDE or `arduino-cli compile --fqbn esp32:esp32:esp32 .`
//   Run host:    ./build/sensord --serial /dev/cu.usbserial-XXXX@921600
//
// Frame:   A5 5A | len:u16 | seq:u32 | payload | crc16:u16   (little-endian)
// Samples: 01 | channel:u16 | t0_us:u64 | period_us:u32 | count:u16 | f32 * count
// SetRate: 02 | rate_hz:u32      RateAck: 03 | rate_hz:u32

#include <Arduino.h>

static const int kAdcPins[] = {34, 35};  // ADC1 pins (safe to use with Wi-Fi on)
static const uint16_t kChannels = sizeof(kAdcPins) / sizeof(kAdcPins[0]);
static const uint32_t kBaud = 921600;
static const uint32_t kFrameIntervalUs = 10000;  // send a frame every 10 ms
static const uint16_t kMaxSamples = 64;          // per channel per frame

static uint32_t g_rate_hz = 250;
static uint32_t g_seq = 0;
static float g_buf[kChannels][kMaxSamples];
static uint16_t g_count = 0;
static uint64_t g_t0_us = 0;
static uint64_t g_next_sample_us = 0;
static uint64_t g_last_frame_us = 0;

// ---- CRC-16/CCITT-FALSE (same as host) --------------------------------------
static uint16_t crc16(const uint8_t* d, size_t n, uint16_t crc = 0xFFFF) {
  while (n--) {
    crc ^= (uint16_t)(*d++) << 8;
    for (int i = 0; i < 8; ++i) crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
  }
  return crc;
}

// ---- framing ---------------------------------------------------------------
static uint8_t g_frame[8 + 1024 + 2];

static void put16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = v >> (8 * i); }
static void put64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = v >> (8 * i); }

static void send_frame(const uint8_t* payload, uint16_t len) {
  g_frame[0] = 0xA5;
  g_frame[1] = 0x5A;
  put16(&g_frame[2], len);
  put32(&g_frame[4], g_seq++);
  memcpy(&g_frame[8], payload, len);
  put16(&g_frame[8 + len], crc16(&g_frame[2], 6 + len));
  Serial.write(g_frame, 8 + len + 2);
}

static void send_samples() {
  if (g_count == 0) return;
  static uint8_t p[17 + 4 * kMaxSamples];
  const uint32_t period = 1000000UL / g_rate_hz;
  for (uint16_t ch = 0; ch < kChannels; ++ch) {
    p[0] = 0x01;
    put16(&p[1], ch);
    put64(&p[3], g_t0_us);
    put32(&p[11], period);
    put16(&p[15], g_count);
    memcpy(&p[17], g_buf[ch], 4 * g_count);  // ESP32 is little-endian IEEE754
    send_frame(p, 17 + 4 * g_count);
  }
  g_count = 0;
}

static void send_ack() {
  uint8_t p[5] = {0x03};
  put32(&p[1], g_rate_hz);
  send_frame(p, 5);
}

// ---- command parser (tiny version of the host's FrameDecoder) ----------------
static uint8_t g_rx[64];
static size_t g_rx_len = 0;

static void handle_rx() {
  while (Serial.available()) {
    if (g_rx_len == sizeof g_rx) g_rx_len = 0;  // junk overflow: start over
    g_rx[g_rx_len++] = (uint8_t)Serial.read();
  }
  size_t pos = 0;
  while (g_rx_len - pos >= 8) {
    if (!(g_rx[pos] == 0xA5 && g_rx[pos + 1] == 0x5A)) { ++pos; continue; }
    uint16_t len = g_rx[pos + 2] | (g_rx[pos + 3] << 8);
    if (len > 32) { ++pos; continue; }
    if (g_rx_len - pos < 8u + len + 2u) break;
    uint16_t want = g_rx[pos + 8 + len] | (g_rx[pos + 9 + len] << 8);
    if (crc16(&g_rx[pos + 2], 6 + len) != want) { ++pos; continue; }
    const uint8_t* pl = &g_rx[pos + 8];
    if (len == 5 && pl[0] == 0x02) {
      uint32_t hz = pl[1] | (pl[2] << 8) | (pl[3] << 16) | ((uint32_t)pl[4] << 24);
      if (hz >= 1 && hz <= 5000) {  // ADC + serial budget on a plain ESP32
        send_samples();            // flush samples taken at the old rate
        g_rate_hz = hz;
        g_next_sample_us = esp_timer_get_time();
      }
      send_ack();
    }
    pos += 8 + len + 2;
  }
  memmove(g_rx, g_rx + pos, g_rx_len - pos);
  g_rx_len -= pos;
}

void setup() {
  Serial.begin(kBaud);
  analogReadResolution(12);
  g_next_sample_us = g_last_frame_us = esp_timer_get_time();
  send_ack();  // tell the host our starting rate
}

void loop() {
  handle_rx();
  const uint64_t now = esp_timer_get_time();
  if (now >= g_next_sample_us && g_count < kMaxSamples) {
    if (g_count == 0) g_t0_us = g_next_sample_us;
    for (uint16_t ch = 0; ch < kChannels; ++ch)
      g_buf[ch][g_count] = analogRead(kAdcPins[ch]) * (3.3f / 4095.0f);  // volts
    ++g_count;
    g_next_sample_us += 1000000UL / g_rate_hz;
  }
  if (now - g_last_frame_us >= kFrameIntervalUs || g_count == kMaxSamples) {
    send_samples();
    g_last_frame_us = now;
  }
}
