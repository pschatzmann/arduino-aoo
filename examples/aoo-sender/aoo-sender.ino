/***
 * @brief We send a sine wave using Audio Over OSC (AOO) using UDP.
 * Since the generation is too fast, we throttle the generated audio
 * to the correct speed.
 */
#include "WiFi.h"
#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecOpus.h"
#include "AOO.h"

const char *ssid = "ssid";
const char *password = "password";
const int udpPort = 9999;
AudioInfo info(22000, 1, 16);
SineWaveGenerator<int16_t> sineWave;
GeneratedSoundStream<int16_t> sound(sineWave);
Throttle throttled_in(sound);
AOOStreamUDP udp(IPAddress(192, 168, 1, 44), udpPort);
AOOSender aoo_sender(1, udp, 100);
StreamCopy copier(aoo_sender, throttled_in);
OpusAudioEncoder opus;

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

  // --- Simple: start with AudioInfo only (broadcast, PCM) ---
  aoo_sender.begin(info);

  // --- Alternative: start with full configuration ---
  // auto cfg = aoo_sender.defaultConfig();
  // cfg.sample_rate = 22000;
  // cfg.channels = 1;
  // cfg.bits_per_sample = 16;
  //
  // // Use Opus encoder instead of PCM
  // // aoo_sender.setEncoder("opus", opus);
  //
  // // Send each block twice for WiFi loss tolerance
  // // cfg.redundancy = 2;
  //
  // // Fixed audio framing (bytes per block)
  // // cfg.frame_size = 960;
  //
  // // Send to specific sinks at different IPs
  // // cfg.sink_targets = {
  // //   {1, IPAddress(192, 168, 1, 10), 7000},
  // //   {2, IPAddress(192, 168, 1, 11), 7000},
  // // };
  //
  // // Or send to a single sink (no IP needed if same as udp.begin)
  // // cfg.sink_targets = {{1}};
  //
  // throttle.begin(cfg);
  // aoo_sender.begin(cfg);

  Serial.println("started...");
}

void loop() { copier.copy(); }
