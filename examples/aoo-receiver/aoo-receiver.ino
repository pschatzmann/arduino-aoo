/***
 * @brief We receive audio data using Audio Over OSC (AOO) via UDP.
 * We support the decoding of pcm and opus.
 * The AOOReceiver is an AudioStream — use StreamCopy to pull decoded
 * audio and write it to any output. Here the audio is written as CSV to
 * Serial, so that the received data can be checked easily (e.g. with the
 * Serial Plotter): replace the CsvOutput with an I2SStream (or any other
 * output) to play it.
 *
 * A CSV line per frame needs a lot of bandwidth: on a microcontroller use a
 * low sample rate (e.g. 8000 Hz, mono) and a high baud rate.
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
AOOReceiver aoo_receiver(1, udp);  // sink id 1
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
