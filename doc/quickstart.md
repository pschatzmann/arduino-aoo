# Quick Start

## Sender

```cpp
#include "AOO.h"

AOOStreamUDP udp(IPAddress(192, 168, 1, 44), 9999);  // receiver IP and port
AOOSender sender(1, udp);
SineWaveGenerator<int16_t> sineWave;
GeneratedSoundStream<int16_t> sound(sineWave);
Throttle throttled(sound);
StreamCopy copier(sender, throttled);

void setup() {
  AudioInfo info(44100, 2, 16);
  sineWave.begin(info, N_B4);
  throttled.begin(info);
  // when sender and receiver run on the same host, they need different ports
  // udp.setLocalPort(9998);
  sender.begin(info);
}

void loop() { copier.copy(); }
```

## Receiver

```cpp
#include "AOO.h"

AOOStreamUDP udp(9999);
AOOReceiver receiver(1, udp);
I2SStream i2s;
StreamCopy copier(i2s, receiver);  // receiver is the audio SOURCE
AudioInfo info(44100, 2, 16);

void setup() {
  auto cfg_i2s = i2s.defaultConfig(TX_MODE);
  cfg_i2s.copyFrom(info);
  i2s.begin(cfg_i2s);
  // the output format: all sources are converted to it
  auto cfg = receiver.defaultConfig();
  cfg.copyFrom(info);
  receiver.begin(cfg);
}

void loop() { copier.copy(); }
```

## Receiver with FreeRTOS Task (ESP32)

```cpp
#include "AOO.h"

AOOStreamUDP udp(9999);
AOOReceiverTask receiver(1, udp);  // UDP processing on core 0
I2SStream i2s;
StreamCopy copier(i2s, receiver);

void setup() {
  i2s.begin(i2s.defaultConfig(TX_MODE));
  receiver.setTaskConfig(8192, 1, 0);  // stack, priority, core
  receiver.begin();  // output format: see Receiver above
}

void loop() { copier.copy(); }
```

## Using Opus Codec

PCM is built-in. Register Opus for compressed streaming. Like the official
aoo library we use one Opus stream per channel (multistream with separate
channels):

```cpp
#include "AudioTools/AudioCodecs/CodecOpusMultiStream.h"

// Sender: Opus encoding (each sender needs its own encoder)
OpusMultiStreamAudioEncoder opus;
opus.config().default_channel_mapping = OPUS_CHANNEL_MAPPING_SEPARATE;
opus.config().frame_sizes_ms_x2 = OPUS_FRAMESIZE_20_MS;
sender.setEncoder("opus", opus);

// Receiver: Opus decoding (a new decoder for each source)
receiver.addDecoder("opus", []() {
  auto* decoder = new OpusMultiStreamAudioDecoder();
  decoder->config().default_channel_mapping = OPUS_CHANNEL_MAPPING_SEPARATE;
  return (AudioDecoder*)decoder;
});
```

## Sender Configuration

```cpp
AOOSender sender(1, udp);
auto cfg = sender.defaultConfig();
cfg.sample_rate = 44100;
cfg.channels = 2;
cfg.bits_per_sample = 16;
cfg.sink_targets = {{1, IPAddress(192,168,1,10), 7000}};
cfg.redundancy = 2;         // send each block twice for loss tolerance
cfg.max_frame_size = 1400;  // split blocks larger than MTU
cfg.ping_interval_ms = 1000;
sender.begin(cfg);
```

## Receiver Configuration

```cpp
AOOReceiver receiver(1, udp);
auto cfg = receiver.defaultConfig();
cfg.sample_rate = 48000;
cfg.channels = 2;
cfg.bits_per_sample = 16;
cfg.buffer_depth = 10;           // encoded segments to buffer
cfg.adaptive_resampling = true;  // compensate clock drift
cfg.max_resample_percent = 0.5;  // max. playback rate adjustment
cfg.recovery_wait_ms = 10;       // resend request timing
cfg.recovery_max_requests = 1;   // max retries per gap, 0 = no resends
cfg.min_quality_percent = 80;    // suppress resends below this quality
cfg.stream_timeout_ms = 5000;    // remove silent sources
cfg.output_follows_source = false;  // true: use the format of the 1st source
receiver.begin(cfg);
```

See [Choosing the Error Correction](architecture.md#choosing-the-error-correction) for measured recommendations.
