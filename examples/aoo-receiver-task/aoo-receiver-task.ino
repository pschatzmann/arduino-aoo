/***
 * @brief Same as aoo-receiver, but using AOOReceiverTask instead of
 * AOOReceiver: UDP message reception (parsing /start, /data, /stop, resend
 * requests, ...) runs in its own task instead of being driven from loop().
 *
 * On ESP32's dual-core boards this lets message reception run on one core
 * while I2S/audio decoding runs on the other, which avoids UDP packet loss
 * when loop() is busy for a while (e.g. writing to slow storage). On other
 * platforms it still works, just without the second-core benefit.
 *
 * AOOReceiverTask is a drop-in replacement for AOOReceiver: same
 * constructor, defaultConfig()/begin() and readBytes()/available() via
 * StreamCopy. The only addition is setTaskConfig() to tune the task's stack
 * size, priority and CPU core.
 *
 * The audio is written as CSV to Serial, so that the received data can be
 * checked easily: replace the CsvOutput with an I2SStream (or any other
 * output) to play it.
 */
#include "AOO.h"
#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecOpusMultiStream.h"
#include "WiFi.h"

const char* ssid = "SSID";
const char* password = "password";
const int udpPort = 9999;  // port of this receiver (sink)
AudioInfo info(48000, 2, 16);
AOOStreamUDP udp(udpPort);
AOOReceiverTask aoo_receiver(1, udp);  // sink id 1
CsvOutput<int16_t> out(Serial);
// the receiver provides data on demand: limit the reads to real time
Throttle throttle(out);
StreamCopy copier(throttle, aoo_receiver);  // receiver is the audio SOURCE

// Each sender instance needs to have it separate decoder instance!
AudioDecoder* createOpusDecoder() {
  OpusMultiStreamAudioDecoder* decoder = new OpusMultiStreamAudioDecoder();
  decoder->config().default_channel_mapping = OPUS_CHANNEL_MAPPING_SEPARATE;
  return decoder;
}

void setup() {
  Serial.begin(115200);
  // log to Serial only for warnings: the CSV output also goes to Serial
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);

  // Connect to WiFi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  WiFi.setSleep(false);

  // start the output
  out.begin(info);
  throttle.begin(info);

  // register decoders
  aoo_receiver.addDecoder("opus", createOpusDecoder);

  // Run message reception on core 0, leaving core 1 free for loop()/I2S
  // (stack size, priority, core - defaults shown here)
  aoo_receiver.setTaskConfig(8192, 1, 0);

  // --- Set the output format and start; uncomment below to tune further ---
  auto cfg = aoo_receiver.defaultConfig();
  // Define the audio format to be used for the output: all sources are
  // converted to it
  cfg.copyFrom(info);

  // Buffer depth for encoded segments (also serves as jitter buffer)
  // cfg.buffer_depth = 10;

  // Adaptive resampling to compensate for clock drift
  // cfg.adaptive_resampling = true;

  // Auto-remove sources that stop sending after 5 seconds
  // cfg.stream_timeout_ms = 5000;

  // Tune packet recovery timing
  // cfg.recovery_wait_ms = 30;
  // cfg.recovery_max_requests = 5;

  aoo_receiver.begin(cfg);
}

void loop() {
  copier.copy();  // pulls from aoo_receiver.readBytes() -> writes to out
}
