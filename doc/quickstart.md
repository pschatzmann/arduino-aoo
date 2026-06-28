# Quick Start

## Sender

```cpp
#include "AOO.h"

AOOStreamUDP udp(IPAddress(192, 168, 1, 44), 9999);
AOOSender sender(1, udp);
SineWaveGenerator<int16_t> sineWave;
GeneratedSoundStream<int16_t> sound(sineWave);
Throttle throttled(sound);
StreamCopy copier(sender, throttled);

void setup() {
  AudioInfo info(44100, 2, 16);
  sineWave.begin(info, N_B4);
  throttled.begin(info);
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

void setup() {
  i2s.begin(i2s.defaultConfig(TX_MODE));
  receiver.begin();  // auto-detects format from sender
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
  receiver.setTaskConfig(4096, 1, 0);  // stack, priority, core
  receiver.begin();
}

void loop() { copier.copy(); }
```

## Using Opus Codec

PCM is built-in. Register Opus for compressed streaming:

```cpp
// Sender: Opus encoding
OpusAudioEncoder opus;
sender.setEncoder("opus", opus);

// Receiver: Opus decoding
receiver.addDecoder("opus", []() {
  return (AudioDecoder*)new OpusAudioDecoder();
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
cfg.recovery_wait_ms = 20;       // resend request timing
cfg.recovery_max_requests = 1;   // max retries per gap
cfg.stream_timeout_ms = 5000;    // remove silent sources
receiver.begin(cfg);
```
