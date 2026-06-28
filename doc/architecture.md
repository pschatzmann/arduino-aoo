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
cfg.recovery_wait_ms = 20;       // resend request timing
cfg.recovery_max_requests = 1;   // max retries per gap
cfg.min_quality_percent = 98.0;  // suppress resends below this
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

### Gap Handling

When a segment is missing during read, silence is returned instead of stalling. This ensures the mixer keeps running even with packet loss.

### Quality-Based Resend Control

Resend requests are only sent when quality is above `min_quality_percent` (default 98%). This prevents flooding the network during periods of high loss, which would make congestion worse.

Sources below `min_source_quality` (default 50%) are automatically removed from the mixer.

## AOOReceiverTask (ESP32)

Subclass of `AOOReceiver` that processes UDP messages in a separate FreeRTOS task. This eliminates packet loss by decoupling network reception from audio decoding:

- **Core 0**: FreeRTOS task continuously reads UDP, parses messages, stores encoded data
- **Core 1**: Audio thread pulls decoded data via `readBytes()`

A mutex protects individual buffer operations (fine-grained locking).

```cpp
AOOReceiverTask receiver(1, udp);
receiver.setTaskConfig(4096, 1, 0);  // stack, priority, core
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

`AOOClockSync` estimates wall-clock offset and round-trip time using NTP-style calculations from ping/pong timestamps. Accessible via `sender.clockSync()` or `receiver.clockSync()`.

## Adaptive Resampling

When enabled, the receiver measures the effective source sample rate from received data timestamps and adjusts the `FormatConverterStream`'s internal resampler to compensate for crystal-oscillator drift between sender and receiver.

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
