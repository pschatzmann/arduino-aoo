/**
 * @brief Streaming test over UDP for the desktop: sends a sine wave or
 * receives a stream, pulling the audio in real time like an I2S output,
 * writes the received audio as CSV and reports statistics. Configured by
 * environment variables (defaults in brackets):
 * - AOO_MODE: send, receive or receive-task
 * - AOO_PORT [9999 receiver, 9998 sender], AOO_REMOTE_PORT [9999],
 *   AOO_REMOTE_IP [127.0.0.1]
 * - AOO_SECONDS [5]
 * - AOO_CODEC: pcm or opus [pcm], AOO_RATE [48000], AOO_CHANNELS [1]
 * - AOO_BLOCK: samples per block (pcm) or frame ms (opus) [256 / 20]
 * - AOO_FREQ: sine frequency [440]
 * - sender: AOO_REDUNDANCY [1], AOO_RESEND_BUFFER [50]
 * - receiver: AOO_BUFFER_DEPTH [10], AOO_RESEND (max requests, 0 = off) [1],
 *   AOO_RECOVERY_WAIT_MS [10], AOO_ADAPTIVE [1], AOO_CSV (file) []
 */
#include "AOO.h"
#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecOpusMultiStream.h"

int envInt(const char *name, int def) {
  const char *value = getenv(name);
  return value != nullptr ? atoi(value) : def;
}

const char *envStr(const char *name, const char *def) {
  const char *value = getenv(name);
  return value != nullptr ? value : def;
}

/// Sleeps until the indicated time in us (real time pacing)
void waitUntil(uint64_t due) {
  while (micros() < due) delayMicroseconds(200);
}

AudioDecoder *createOpusDecoder() {
  auto *decoder = new OpusMultiStreamAudioDecoder();
  decoder->config().default_channel_mapping = OPUS_CHANNEL_MAPPING_SEPARATE;
  return decoder;
}

int send(AudioInfo info, bool opus, int block, int seconds) {
  IPAddress remote_ip;
  remote_ip.fromString(envStr("AOO_REMOTE_IP", "127.0.0.1"));
  AOOStreamUDP<WiFiUDP> udp(remote_ip, envInt("AOO_REMOTE_PORT", 9999));
  udp.setLocalPort(envInt("AOO_PORT", 9998));
  // the encoder must exist as long as the sender uses it
  OpusMultiStreamAudioEncoder encoder;
  AOOSender sender(1, udp);
  int frames = block;
  if (opus) {
    auto &ocfg = encoder.config();
    ocfg.complexity = 1;
    ocfg.default_channel_mapping = OPUS_CHANNEL_MAPPING_SEPARATE;
    switch (block) {
      case 10: ocfg.frame_sizes_ms_x2 = OPUS_FRAMESIZE_10_MS; break;
      case 40: ocfg.frame_sizes_ms_x2 = OPUS_FRAMESIZE_40_MS; break;
      case 60: ocfg.frame_sizes_ms_x2 = OPUS_FRAMESIZE_60_MS; break;
      default: ocfg.frame_sizes_ms_x2 = OPUS_FRAMESIZE_20_MS; block = 20;
    }
    sender.setEncoder("opus", encoder);
    frames = info.sample_rate * block / 1000;
  }
  auto cfg = sender.defaultConfig();
  cfg.copyFrom(info);
  cfg.sink_targets.push_back(AOOSinkTarget(1));
  cfg.redundancy = envInt("AOO_REDUNDANCY", 1);
  cfg.buffer_size = envInt("AOO_RESEND_BUFFER", 50);
  if (!sender.begin(cfg)) return 1;

  SineWaveGenerator<int16_t> sine(16000);
  sine.begin(info, envInt("AOO_FREQ", 440));
  std::vector<int16_t> buf(frames * info.channels);
  uint64_t start = micros();
  int blocks = (uint64_t)seconds * info.sample_rate / frames;
  for (int b = 0; b < blocks; b++) {
    for (int i = 0; i < frames; i++) {
      int16_t v = sine.readSample();
      for (int c = 0; c < info.channels; c++) buf[i * info.channels + c] = v;
    }
    sender.write((const uint8_t *)buf.data(), buf.size() * 2);
    waitUntil(start + (uint64_t)(b + 1) * frames * 1000000ULL / info.sample_rate);
  }
  sender.end();
  fprintf(stderr, "sender: blocks=%d frames_per_block=%d bytes=%u rtt_us=%lu\n",
          blocks, frames, (unsigned)sender.getTotalBytesSent(),
          (unsigned long)sender.clockSync().rttMicros());
  return 0;
}

int receive(AudioInfo info, bool task, int seconds) {
  AOOStreamUDP<WiFiUDP> udp(envInt("AOO_PORT", 9999));
  AOOReceiver plain(1, udp);
  AOOReceiverTask with_task(1, udp);
  AOOReceiver &receiver = task ? (AOOReceiver &)with_task : plain;
  receiver.addDecoder("opus", createOpusDecoder);
  auto cfg = receiver.defaultConfig();
  cfg.copyFrom(info);
  cfg.buffer_depth = envInt("AOO_BUFFER_DEPTH", 10);
  cfg.recovery_max_requests = envInt("AOO_RESEND", 1);
  cfg.recovery_wait_ms = envInt("AOO_RECOVERY_WAIT_MS", cfg.recovery_wait_ms);
  cfg.adaptive_resampling = envInt("AOO_ADAPTIVE", 1) != 0;
  if (!receiver.begin(cfg)) return 1;

  FILE *csv = nullptr;
  const char *csv_path = envStr("AOO_CSV", "");
  if (strlen(csv_path) > 0) csv = fopen(csv_path, "w");

  // pull like an I2S output: 256 frames every 256 / sample_rate seconds
  const int frames = 256;
  std::vector<int16_t> buf(frames * info.channels);
  // underruns are counted from the first to the last received block
  long underrun_frames = 0, output_frames = 0, pending_underrun = 0;
  long pending_output = 0;
  bool started = false;
  uint64_t start = micros();
  int blocks = (uint64_t)seconds * info.sample_rate / frames;
  for (int b = 0; b < blocks; b++) {
    size_t bytes = 0;
    if (receiver.available() > 0) {
      bytes = receiver.readBytes((uint8_t *)buf.data(), buf.size() * 2);
    }
    int got = bytes / 2 / info.channels;
    if (got > 0) started = true;
    if (started) {
      // the output plays silence for the missing frames
      pending_underrun += frames - got;
      pending_output += frames;
      if (got > 0) {
        underrun_frames += pending_underrun;
        output_frames += pending_output;
        pending_underrun = pending_output = 0;
      }
      if (csv) {
        for (int i = 0; i < frames; i++)
          fprintf(csv, "%d\n", i < got ? buf[i * info.channels] : 0);
      }
    }
    waitUntil(start + (uint64_t)(b + 1) * frames * 1000000ULL / info.sample_rate);
  }
  if (csv) fclose(csv);
  auto &st = receiver.stats();
  auto &ms = receiver.messageStats();
  fprintf(stderr,
          "receiver: output_s=%.2f underrun_ms=%.0f stored=%u gaps=%u "
          "resend_requests=%u start_requests=%u osc_start=%u data=%u "
          "parse_errors=%u assembly=%u/%u single=%u\n",
          (double)output_frames / info.sample_rate,
          underrun_frames * 1000.0 / info.sample_rate, st.segments_stored,
          receiver.xrunCount(), st.resend_requests, st.start_requests,
          ms.osc_start, ms.osc_data + ms.binary_data, ms.parse_errors,
          st.assembly_completed, st.assembly_started, st.single_frame);
  if (task) with_task.end();
  return 0;
}

void setup() {
  AudioToolsLogger.begin(Serial, (AudioToolsLogLevel)envInt(
                                     "AOO_LOG_LEVEL",
                                     (int)AudioToolsLogLevel::Warning));
  const char *mode = envStr("AOO_MODE", "");
  bool opus = strcmp(envStr("AOO_CODEC", "pcm"), "opus") == 0;
  AudioInfo info(envInt("AOO_RATE", 48000), envInt("AOO_CHANNELS", 1), 16);
  int seconds = envInt("AOO_SECONDS", 5);
  int rc = 2;
  if (strcmp(mode, "send") == 0)
    rc = send(info, opus, envInt("AOO_BLOCK", opus ? 20 : 256), seconds);
  if (strcmp(mode, "receive") == 0) rc = receive(info, false, seconds);
  if (strcmp(mode, "receive-task") == 0) rc = receive(info, true, seconds);
  fflush(stdout);
  exit(rc);
}

void loop() {}
