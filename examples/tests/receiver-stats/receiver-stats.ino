/**
 * @brief Receives a sine wave via AOO (UDP) and analyzes it on the device
 * instead of outputting it: the audio is pulled in real time like an I2S
 * output (256 frames per block period) and every 10 seconds a summary is
 * printed: received audio, underruns (silence the output would play),
 * estimated frequency, rms and the receiver statistics. Use it to test the
 * reception independently of the output (a CSV output can not keep up with
 * higher sample rates on a microcontroller).
 */
#include "AOO.h"
#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecOpusMultiStream.h"
#include "WiFi.h"

#ifndef USE_TASK
#define USE_TASK 0  // 1: use AOOReceiverTask
#endif
#ifndef SAMPLE_RATE
#define SAMPLE_RATE 48000
#endif
#ifndef RESEND
#define RESEND 1  // max resend requests per missing packet, 0 = off
#endif
#ifndef BUFFER_DEPTH
#define BUFFER_DEPTH 10
#endif

const char *ssid = "SSID";
const char *password = "password";
const int udpPort = 9999;
const int frames = 256;
AudioInfo info(SAMPLE_RATE, 1, 16);
AOOStreamUDP udp(udpPort);
#if USE_TASK
AOOReceiverTask aoo_receiver(1, udp);
#else
AOOReceiver aoo_receiver(1, udp);
#endif
int16_t buffer[frames];

// analysis of the current 10 second period
struct Period {
  long output_frames = 0, underrun_frames = 0, nonzero = 0, crossings = 0;
  double sum2 = 0;
  int16_t last = 0;
} period;
bool started = false;
uint64_t next_block = 0, next_report = 0;
uint32_t start_ms = 0;

AudioDecoder *createOpusDecoder() {
  auto *decoder = new OpusMultiStreamAudioDecoder();
  decoder->config().default_channel_mapping = OPUS_CHANNEL_MAPPING_SEPARATE;
  return decoder;
}

void report() {
  auto &st = aoo_receiver.stats();
  auto &ms = aoo_receiver.messageStats();
  double freq = period.nonzero > 0
                    ? period.crossings / 2.0 * SAMPLE_RATE / period.nonzero
                    : 0;
  double rms = period.nonzero > 0 ? sqrt(period.sum2 / period.nonzero) : 0;
  Serial.printf(
      "# t=%lus output_s=%.2f underrun_ms=%.0f freq=%.1f rms=%.0f | "
      "sources=%d stored=%u resend_requests=%u start_requests=%u data=%u "
      "heap=%u\n",
      (unsigned long)((millis() - start_ms) / 1000),
      (double)period.output_frames / SAMPLE_RATE,
      period.underrun_frames * 1000.0 / SAMPLE_RATE, freq, rms,
      aoo_receiver.getSourceCount(), st.segments_stored, st.resend_requests,
      st.start_requests, ms.osc_data + ms.binary_data,
#ifdef ESP32
      (unsigned)ESP.getFreeHeap());
#else
      0u);
#endif
  period = Period();
}

void setup() {
  Serial.begin(115200);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  WiFi.setSleep(false);
  Serial.print("# IP ");
  Serial.println(WiFi.localIP());

  aoo_receiver.addDecoder("opus", createOpusDecoder);
  auto cfg = aoo_receiver.defaultConfig();
  cfg.copyFrom(info);
  cfg.recovery_max_requests = RESEND;
  cfg.buffer_depth = BUFFER_DEPTH;
  aoo_receiver.begin(cfg);
  Serial.printf("# receiver: task=%d rate=%d resend=%d buffer_depth=%d\n",
                USE_TASK, SAMPLE_RATE, RESEND, BUFFER_DEPTH);
  start_ms = millis();
  next_block = micros();
  next_report = next_block + 10000000;
}

void loop() {
  // pull one block per block period, like an I2S output
  uint64_t now = micros();
  if (now < next_block) return;
  next_block += (uint64_t)frames * 1000000 / SAMPLE_RATE;
  size_t bytes = 0;
  if (aoo_receiver.available() > 0) {
    bytes = aoo_receiver.readBytes((uint8_t *)buffer, sizeof(buffer));
  }
  int got = bytes / 2;
  if (got > 0) started = true;
  if (started) {
    period.output_frames += frames;
    period.underrun_frames += frames - got;
    for (int i = 0; i < got; i++) {
      int16_t v = buffer[i];
      if (v == 0) continue;
      period.nonzero++;
      period.sum2 += (double)v * v;
      if ((period.last < 0) != (v < 0)) period.crossings++;
      period.last = v;
    }
  }
  if (now >= next_report) {
    next_report += 10000000;
    report();
  }
}
