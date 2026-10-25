/***
 * @brief We receive audio data using Audio Over OSC (AOO) via UDP.
 * We support the decoding of pcm and opus.
 * The AOOReceiver is an AudioStream — use StreamCopy to pull decoded
 * audio and write it to any output: here it is played via I2S (or any other
 * output, e.g. an AudioBoardStream); on the desktop via miniaudio.
 *
 * To check the received data you can replace the I2SStream with a
 * CsvOutput<int16_t> (paced with a Throttle): on a microcontroller use a low
 * sample rate (e.g. 8000 Hz, mono), because a CSV line per frame needs a lot
 * of bandwidth.
 */
#include "AOO.h"
#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecOpusMultiStream.h"
#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include "WiFi.h"
#endif
// #include "AudioTools/AudioLibs/I2SCodecStream.h"

// on desktop we use miniaudio
#if defined(IS_DESKTOP)
#include "AudioTools/AudioLibs/MiniAudioStream.h"
#define I2SStream MiniAudioStream
#endif

const char* ssid = "SSID";
const char* password = "password";
const int udpPort = 9999;  // port of this receiver (sink)
AudioInfo audio_info(48000, 2, 16);
AOOStreamUDP udp(udpPort);
AOOReceiver aoo_receiver(1, udp);  // sink id 1
I2SStream out;  // or any other e.g. I2SCodecStream out(AudioKitEs8388V1);
StreamCopy copier(out, aoo_receiver);  // receiver is the audio SOURCE

// Each sender instance needs to have it separate decoder instance!
AudioDecoder* createOpusDecoder() {
  OpusMultiStreamAudioDecoder* decoder = new OpusMultiStreamAudioDecoder();
  decoder->config().default_channel_mapping = OPUS_CHANNEL_MAPPING_SEPARATE;
  return decoder;
}

void setup() {
  Serial.begin(115200);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);

  // Connect to WiFi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(500);
#if defined(ESP32)
  WiFi.setSleep(false);  // no power saving: less latency and packet loss
#endif
  // the senders need this IP address
  Serial.print("Receiving on ");
  Serial.print(WiFi.localIP());
  Serial.print(":");
  Serial.println(udpPort);

  // start the output
  auto cfg_out = out.defaultConfig(TX_MODE);
  cfg_out.copyFrom(audio_info);
  out.begin(cfg_out);

  // register decoders
  aoo_receiver.addDecoder("opus", createOpusDecoder);

  // --- Set the output format and start; uncomment below to tune further ---
  auto cfg = aoo_receiver.defaultConfig();
  // Define the audio format to be used for the output: all sources are
  // converted to it
  cfg.copyFrom(audio_info);

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
