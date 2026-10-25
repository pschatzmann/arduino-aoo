/***
 * @brief We send a sine wave using Audio Over OSC (AOO) using UDP.
 * Since the generation is too fast, we throttle the generated audio
 * to the correct speed.
 */
#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include "WiFi.h"
#endif
#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecOpusMultiStream.h"
#include "AOO.h"

// When using Opus: increase stack size
#ifdef ESP32
SET_LOOP_TASK_STACK_SIZE(16 * 1024);
#endif

const char *ssid = "ssid";
const char *password = "password";
const int receiverPort = 9999;  // port of the receiver (sink)
const int senderPort = 9998;    // local port of this sender (source)
IPAddress receiverIP(192, 168, 1, 44);
AudioInfo audio_info(48000, 1, 16);
SineWaveGenerator<int16_t> sineWave;
GeneratedSoundStream<int16_t> sound(sineWave);
Throttle throttled_in(sound);
AOOStreamUDP udp(receiverIP, receiverPort);
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
#if defined(ESP32)
  WiFi.setSleep(false);  // no power saving: less latency and packet loss
#endif
  Serial.print("Connected to WiFi. IP address: ");
  Serial.println(WiFi.localIP());

  // the local port must differ from the receiver port, if both run on the
  // same host
  udp.setLocalPort(senderPort);

  // Setup sine wave input
  sineWave.begin(audio_info, N_B4);
  throttled_in.begin(audio_info);

  // Setup Opus encoder for sender: comment out for PCM
  setupEncoder();

  auto cfg = aoo_sender.defaultConfig();
  cfg.copyFrom(audio_info);
  // cfg.sample_rate = 22000;
  // cfg.channels = 1;
  // cfg.bits_per_sample = 16;

  // Send each block twice for WiFi loss tolerance
  // cfg.redundancy = 2;
  
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
