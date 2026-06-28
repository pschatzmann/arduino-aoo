/***
 * @brief We receive audio data using Audio Over OSC (AOO) via UDP.
 * We support the decoding of pcm and opus.
 * The AOOReceiver is an AudioStream — use StreamCopy to pull decoded
 * audio and write it to I2S (or any other output).
 */
#include "AOO.h"
#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecOpusMultiStream.h"
#include "WiFi.h"
// #include "AudioTools/AudioLibs/AudioBoardStream.h"

// on desktop we use PortAudio
#if defined(IS_DESKTOP)
#include "AudioTools/AudioLibs/MiniAudioStream.h"
#define I2SStream MiniAudioStream
#endif

const char* ssid = "SSID";
const char* password = "password";
const int udpPort = 9998;
AudioInfo info(48000, 2, 16);
AOOStreamUDP udp(udpPort);
I2SStream i2s;  // or any other e.g. AudioBoardStream i2s(AudioKitEs8388V1);
AOOReceiver aoo_receiver(1, udp);      // no output in constructor
StreamCopy copier(i2s, aoo_receiver);  // receiver is the audio SOURCE

// Each sender instance needs to have it separate decoder instance!
AudioDecoder* createOpusDecoder() {
  OpusMultiStreamAudioDecoder* decoder = new OpusMultiStreamAudioDecoder();
  decoder->config().default_channel_mapping = OPUS_CHANNEL_MAPPING_SEPARATE;
  return decoder;
}

void setup() {
  Serial.begin(115200);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Info);

  // Connect to WiFi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  WiFi.setSleep(false);
  Serial.print("Connected to WiFi. IP address: ");
  Serial.println(WiFi.localIP());

  // start I2S
  Serial.println("starting I2S...");
  auto cfg_i2s = i2s.defaultConfig(TX_MODE);
  cfg_i2s.copyFrom(info);
  i2s.begin(cfg_i2s);

  // register decoders
  aoo_receiver.addDecoder("opus", createOpusDecoder);

  // --- Simple: start with no config (auto-detect from first message) ---
  auto cfg = aoo_receiver.defaultConfig();
  // Define the audio format to be used for the output (I2S)
  cfg.copyFrom(info);

  //
  // Buffer depth for encoded segments (also serves as jitter buffer)
  // cfg.buffer_depth = 10;

  // Adaptive resampling to compensate for clock drift
  // cfg.adaptive_resampling = true;

  // Auto-remove sources that stop sending after 5 seconds
  // cfg.stream_timeout_ms = 5000;

  // Tune packet recovery timing
  // cfg.recovery_wait_ms = 30;
  // cfg.recovery_max_requests = 5;
  //
  aoo_receiver.begin(cfg);

  Serial.println("started...");
}

void loop() {
  copier.copy();  // pulls from aoo_receiver.readBytes() -> writes to i2s
}
