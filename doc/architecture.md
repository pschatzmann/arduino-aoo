# Architecture & Features

## Naming Convention

The AOO protocol uses "source" and "sink". This library uses **Sender** and **Receiver**:

- **AOOSender** = source. You `write()` audio; it encodes and transmits.
- **AOOReceiver** = sink. You `readBytes()` to pull decoded audio.

Protocol-level message types (e.g. `AOOPingSink`) retain the original AOO terminology.

## AOOSender

Implements `AudioOutput`. Encodes PCM (or any registered codec), splits large blocks into multiple frames, and sends as OSC/binary messages.

### Configuration

```cpp
AOOSender sender(1, udp);
auto cfg = sender.defaultConfig();
cfg.sample_rate = 44100;
cfg.channels = 2;
cfg.bits_per_sample = 16;
cfg.sink_targets = {{1, IPAddress(192,168,1,10), 7000}};
cfg.redundancy = 2;         // send each block twice
cfg.max_frame_size = 1400;  // split blocks larger than this
cfg.ping_interval_ms = 1000;
sender.begin(cfg);
```

### Multi-Receiver Addressing

A single sender can target multiple receivers:

```cpp
cfg.sink_targets = {
  {1, IPAddress(192, 168, 1, 10), 7000},
  {2, IPAddress(192, 168, 1, 11), 7000},
};
```

Receivers that send `AOOInvite` are automatically added with their IP resolved from the incoming packet.

### Redundancy

Each audio block can be sent multiple times to tolerate packet loss:

```cpp
cfg.redundancy = 2;  // send each block twice
```

The receiver deduplicates via sequence number.

## AOOReceiver

Implements `AudioStream` with pull-based `readBytes()`. Receives encoded data, buffers it per source, and decodes on demand.

### Architecture

Each remote source gets its own processing pipeline:

```
UDP → AOOMessageHandler → store encoded data in IndexedRingBuffer
                                    ↓
readBytes() → InputMixer ← [per source: BufferView → Decoder → FormatConverter]
```

### Configuration

```cpp
AOOReceiver receiver(1, udp);
auto cfg = receiver.defaultConfig();
cfg.sample_rate = 48000;
cfg.channels = 2;
cfg.bits_per_sample = 16;
cfg.buffer_depth = 10;           // encoded segments to buffer
cfg.adaptive_resampling = true;  // compensate clock drift
cfg.recovery_wait_ms = 10;       // resend request timing
cfg.recovery_max_requests = 1;   // max retries per gap
cfg.min_quality_percent = 80.0;  // suppress resends below this
cfg.min_source_quality = 50.0;   // drop sources below this
cfg.stream_timeout_ms = 5000;    // remove silent sources
receiver.begin(cfg);
```

### Buffer and Priming

Encoded data is stored in an `IndexedRingBuffer` per source. Data is only provided to the decoder once the buffer is half full (priming), giving time for:

- Out-of-order packet arrival
- Gap detection and resend requests
- Resend round-trip completion

The priming delay = `(buffer_depth / 2) × block_time`. With the default of 10 and typical 10ms blocks: 50ms.

### Decoding

On read, the encoded segments are taken one by one from the buffer and written to the decoder, so that a decoder like Opus gets exactly one packet per write. The decoded audio is converted to the output format and queued for `readBytes()`.

### Gap Handling

When a segment is missing during read, silence of the size of the last decoded segment is returned instead of stalling. This ensures the mixer keeps running even with packet loss. When the next segment has simply not arrived yet (underrun), nothing is skipped: it is used as soon as it arrives.

### Quality-Based Resend Control

Resend requests are only sent when the quality of the last 100 blocks is above `min_quality_percent` (default 80%). This prevents flooding the network during periods of high loss, which would make congestion worse. Requests for segments which the reader has already passed (and replaced by silence) are discarded.

Sources below `min_source_quality` (default 50%) are automatically removed from the mixer.

### Choosing the Error Correction

Measured with a 440 Hz sine (48 kHz mono): sender and receiver on the desktop with a lossy UDP proxy (random loss in both directions, 5 ms jitter, 5 s per case; without loss both receivers were also run for 60 s without any gap), and an ESP32-S3 receiving over WiFi (60 s per case). "Gaps" is the silence the output had to play.

| Desktop, 5 s, random loss | 5% | 10% | 20% |
|---|---|---|---|
| Opus 20 ms, no error correction | 280 ms | 540 ms | 900 ms |
| Opus 20 ms, resend (default) | 0 ms | 100 ms | 340 ms |
| Opus 20 ms, redundancy 2 | 20 ms | 40 ms | 140 ms |
| Opus 20 ms, resend + redundancy 2 | 0 ms | 20 ms | 60 ms |
| PCM 256 frames, no error correction | 280 ms | 510 ms | 780 ms |
| PCM 256 frames, resend, buffer_depth 10 | 280 ms | 540 ms | 870 ms |
| PCM 256 frames, redundancy 2 | 20 ms | 85 ms | 190 ms |

Recommendations:

- **Resend requests** (`recovery_max_requests` = 1, default) cost almost no bandwidth and work well when the buffer delay `(buffer_depth / 2) × block_time` is clearly larger than `recovery_wait_ms` + round trip time: e.g. Opus with 20 ms frames and the default `buffer_depth` of 10 (100 ms). With small PCM blocks (256 frames at 48 kHz = 5.3 ms, i.e. 27 ms delay with `buffer_depth` 10) the resent packets arrive too late: use `buffer_depth` = 20 or more (gaps at 5% loss and 20 ms jitter: 282 ms -> 107 ms), or bigger blocks.
- **Redundancy** (`redundancy` = 2 on the sender) is the most robust against random loss, but doubles the bandwidth: use it with Opus, or with PCM at low sample rates.
- **Both together** give the best result on lossy links.
- On WiFi (ESP32-S3 receiving 48 kHz PCM or Opus for 60 s) 97-99.9% of the packets arrived: losses typically come in bursts of up to about a second, which none of the methods can fully cover; a bigger `buffer_depth` only helps against jitter. Opus needs much less bandwidth and was the most robust choice.
- **Clock drift**: `adaptive_resampling` adjusts the playback rate by up to `max_resample_percent` (default 0.5%) to keep the buffer half full.
- **Output speed**: the receiver is pulled by its output. An output which is slower than real time (e.g. a CSV output on a microcontroller at 48 kHz) makes the buffer overrun, which looks like packet loss: use a low sample rate for the CSV output or the `receiver-stats` test sketch to measure the reception.

## AOOReceiverTask (ESP32)

Subclass of `AOOReceiver` that processes UDP messages in a separate FreeRTOS task. This eliminates packet loss by decoupling network reception from audio decoding:

- **Core 0**: FreeRTOS task continuously reads UDP, parses messages, stores encoded data
- **Core 1**: Audio thread pulls decoded data via `readBytes()`

A mutex protects each source's encoded buffer, a recursive mutex the list of sources and the mixer. The task needs a stack of 8192 bytes (default). `end()` stops the task in a state where it does not hold a lock, so the receiver can be started again.

```cpp
AOOReceiverTask receiver(1, udp);
receiver.setTaskConfig(8192, 1, 0);  // stack, priority, core
receiver.begin();
```

## AOOMessageHandler

Shared message parsing and routing used by both sender and receiver. Handles:

- **Internally**: ping/pong exchange and clock synchronization
- **Via callbacks**: start, stop, data, invite, uninvite, decline (differ between sender/receiver)

Both `AOOSender` and `AOOReceiver` implement `AOOMessageListener` to receive dispatched messages.

## Protocol Messages

| Message | Direction | Purpose |
|---------|-----------|---------|
| `AOOStart` | Sender → Receiver | Start stream (codec, sample rate, channels, block size) |
| `AOORequestStart` | Receiver → Sender | Request sender to (re-)send start |
| `AOOStopSink` | Sender → Receiver | Stop stream |
| `AOOStopSource` | Receiver → Sender | Request sender to stop |
| `AOOData` | Sender → Receiver | Audio data with sequence number and timestamp |
| `AOOResendData` | Receiver → Sender | Request retransmission of missing packets |
| `AOOPingSink/Source` | Both directions | Keep-alive + clock sync |
| `AOOPongSink/Source` | Both directions | Pong reply with NTP-style timestamps |
| `AOOInvite` | Receiver → Sender | Request sender to start streaming |
| `AOOUninvite` | Receiver → Sender | Request sender to stop streaming |
| `AOODecline` | Sender → Receiver | Refuse invitation |

Both OSC and binary message formats are supported. The binary format (used by the official aoo library) is auto-detected.

## Clock Synchronization

`AOOClockSync` estimates wall-clock offset and round-trip time using NTP-style calculations from ping/pong timestamps. Like in the official aoo library all timestamps are NTP timetags (`aoo_ntp_now()`, based on the system time). Accessible via `sender.clockSync()` or `receiver.clockSync()`.

## Adaptive Resampling

When enabled, the receiver compensates the clock drift between sender and receiver by adjusting the playback rate so that the buffer stays half full: every `resample_update_ms` the (smoothed) fill level is compared with the target and the source sample rate of the `FormatConverterStream` is changed proportionally, by at most `max_resample_percent` (default 0.5%).

## Transport

| Class | Description |
|-------|-------------|
| `AOOStreamUDP` | UDP. Each `write()` = one packet. Supports unicast and multicast. |
| `AOOStreamSerial` | Serial with HDLC framing for point-to-point links. |

Custom transports can subclass `AOOStream`.

## File Structure

```
src/
  AOO.h                    — Main include
  AOOSender.h              — Sender (AudioOutput)
  AOOReceiver.h            — Receiver (AudioStream)
  AOOReceiverTask.h        — Receiver with FreeRTOS task
  AOOConfig.h              — Configuration structs
  aoo/
    AOOProtocol.h          — OSC + binary message types
    AOOMessageHandler.h    — Shared message parsing + ping/pong
    AOOBuffers.h           — IndexedRingBuffer
    AOOBufferView.h        — Buffer stream view + statistics
    AOOStream.h            — Transport abstraction
    AOOClockSync.h         — NTP-style clock sync
    AOOPacketRecovery.h    — Gap tracking + resend scheduling
```
