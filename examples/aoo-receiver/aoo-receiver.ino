/***
 * @brief We receive audio data using Audio Over OSC (AOO) via UDP.
 * We support the decoding of pcm and opus.
 * The audio output is done to the I2SStream.
 */

#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecOpus.h" // https://github.com/pschatzmann/arduino-libopus
#include "AOO.h"
// #include "AudioTools/AudioLibs/AudioBoardStream.h"

// on desktop we use PortAudio
#if defined(IS_DESKTOP)
#  include "AudioTools/AudioLibs/MiniAudioStream.h"
# define I2SStream MiniAudioStream
#endif

const char* ssid = "SSID";
const char* password = "password";
AOOStreamUDP udp;
const int udpPort = 7000;
I2SStream i2s;  // or any other e.g. AudioBoardStream i2s(AudioKitEs8388V1);
AOOSink aoo_sink(1, udp, i2s); // or AOOSinkSingle

void setup() {
  Serial.begin(115200);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Info);

  // Connect to WiFi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(500);

  // start UDP receive
  udp.begin(udpPort);

  // start I2S
  Serial.println("starting I2S...");
  auto config = i2s.defaultConfig(TX_MODE);
  i2s.begin(config);

  // register decoders
  aoo_sink.addDecoder("opus",[]() { return (AudioDecoder *)new OpusAudioDecoder(); });

  // --- Simple: start with no config (auto-detect from first message) ---
  aoo_sink.begin();

  // --- Alternative: start with full configuration ---
  // auto cfg = aoo_sink.defaultConfig();
  // cfg.sample_rate = 44100;
  // cfg.channels = 2;
  // cfg.bits_per_sample = 16;
  //
  // // Jitter buffer: hold 5 blocks before releasing
  // // cfg.jitter_buffer_depth = 5;
  // // cfg.jitter_buffer_block_size = 4096;
  //
  // // Adaptive resampling to compensate for clock drift
  // // cfg.adaptive_resampling = true;
  //
  // // Auto-remove sources that stop sending after 5 seconds
  // // cfg.stream_timeout_ms = 5000;
  //
  // // Tune packet recovery timing
  // // cfg.recovery_wait_ms = 30;
  // // cfg.recovery_max_requests = 5;
  //
  // aoo_sink.begin(cfg);
  //
  // // Invite a specific source to start streaming to us
  // // aoo_sink.invite(1);

  Serial.println("started...");
}

void loop() {
  aoo_sink.copy();
}
