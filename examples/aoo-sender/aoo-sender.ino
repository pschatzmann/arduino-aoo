/***
 * @brief We send a sine wave using Audio Over OSC (AOO) using UDP.
 * Since the generation is too fast, we throttle the generated audio
 * to the correct speed.
 */
#include "WiFi.h"
#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecOpusMultiStream.h"
#include "AOO.h"

// When using Opus: increase stack size
SET_LOOP_TASK_STACK_SIZE(16 * 1024);


const char *ssid = "ssid";
const char *password = "password";
const int udpPort = 9999;
AudioInfo info(22000, 1, 16);
SineWaveGenerator<int16_t> sineWave;
GeneratedSoundStream<int16_t> sound(sineWave);
Throttle throttled_in(sound);
AOOStreamUDP udp(IPAddress(192, 168, 1, 44), udpPort);
AOOSender aoo_sender(1, udp);
StreamCopy copier(aoo_sender, throttled_in);
OpusMultiStreamAudioEncoder opus;

void setupEncoder() {
  auto& ocfg = opus.config();
  ocfg.complexity = 1;  // much lighter, still decent quality
  ocfg.frame_sizes_ms_x2 = OPUS_FRAMESIZE_40_MS;
  ocfg.default_channel_mapping = OPUS_CHANNEL_MAPPING_SEPARATE;
  aoo_sender.setEncoder("opus", opus);
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

  // Setup sine wave input
  sineWave.begin(info, N_B4);
  throttled_in.begin(info);

  // Setup Opus encoder for sender
  setupEncoder();

  auto cfg = aoo_sender.defaultConfig();
  cfg.copyFrom(info);
  // cfg.sample_rate = 22000;
  // cfg.channels = 1;
  // cfg.bits_per_sample = 16;

  // Send each block twice for WiFi loss tolerance
  // cfg.redundancy = 2;
  
  // Fixed audio framing (bytes per block)
  // cfg.frame_size = 960;
  
  // Send to specific sinks at different IPs
  // cfg.sink_targets = {
  //   {1, IPAddress(192, 168, 1, 10), 7000},
  //   {2, IPAddress(192, 168, 1, 11), 7000},
  // };
  
  // Or send to a single sink (no IP needed if same as udp.begin)
  // cfg.sink_targets = {{1}};
  //
  aoo_sender.begin(cfg);

  Serial.println("started...");
}

void loop() { copier.copy(); }
