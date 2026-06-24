# AoO - Audio Over OSC

"Audio over OSC" aka AoO is aimed to be a message based audio system inspired by Open Sound Control OSC as a syntax format. It is dedicated to send audio in real time from arbitrary senders to arbitrary receivers on demand.

This is a header-only C++ implementation that is using the Arduino Networking API for communications and the functionality of the Arduino Audio Tools for the audio processing.

#### Documentation

- [AoO V2.0 Protocol](https://git.iem.at/aoo/aoo/-/blob/master/doc/aoo_protocol.md) 
- [OSC V1.0 Specification](https://opensoundcontrol.stanford.edu/spec-1_0.html)


## Features

### Naming Convention: Sender and Receiver

The AOO protocol uses the terms "source" and "sink", but this library uses **Sender** and **Receiver** for clarity:

- **AOOSender** = sends audio over the network. You `write()` audio into it, and it encodes and transmits.
- **AOOReceiver** = receives audio from the network. It decodes and plays it.

The protocol-level message types (e.g. `AOOSinkTarget`, `AOOPingSink`) retain the original AOO terminology.

### Core Classes

| Class | Description |
|-------|-------------|
| `AOOSender` | Sends audio over the network. Encodes PCM (or any codec), splits into OSC messages, handles ping/pong keep-alive and resend requests. Implements `AudioOutput` so it plugs directly into the AudioTools `StreamCopy` pipeline. |
| `AOOReceiver` | Receives audio from one or more senders, decodes it, mixes multiple streams, and writes to an audio output. Call `copy()` in your `loop()` to process incoming messages. |
| `AOOReceiverSingle` | Simplified single-source variant of `AOOReceiver` with lower memory usage. Buffers decoded blocks and copies them to the output via `StreamCopy`. |

### Configuration

Both sender and receiver accept a typed configuration struct that extends `AudioInfo`:

```cpp
AOOSender sender(1, udp, 100);
auto cfg = sender.defaultConfig();
cfg.sample_rate = 44100;
cfg.channels = 2;
cfg.bits_per_sample = 16;
cfg.sink_targets = {{2}};
cfg.redundancy = 2;
sender.begin(cfg);
```

| Struct | Key fields |
|--------|-----------|
| `AOOSenderConfig` | `id`, `sink_targets`, `buffer_time_ms`, `max_frame_size`, `redundancy`, `codec_delay_samples`, `length_prefix`, `log_osc` |
| `AOOReceiverConfig` | `id`, `jitter_buffer_depth`, `adaptive_resampling`, `recovery_wait_ms`, `recovery_max_requests`, `mixer_size`, `stream_timeout_ms`, `length_prefix`, `log_osc` |

The plain `begin(AudioInfo)` and `begin()` overloads still work for simple setups.

### Protocol Messages

All AOO v2.0 message types are implemented as structs that can serialize to and parse from OSC binary data. The struct names use the original AOO protocol terminology (source/sink):

| Message | Direction | Purpose |
|---------|-----------|---------|
| `AOOStart` | Sender -> Receiver | Start a stream (codec, sample rate, channels, block size) |
| `AOORequestStart` | Receiver -> Sender | Request the sender to (re-)send the start message |
| `AOOStopSink` | Sender -> Receiver | Stop a stream |
| `AOOStopSource` | Receiver -> Sender | Request the sender to stop |
| `AOOData` | Sender -> Receiver | Audio data with sequence number, timestamp, multi-frame support |
| `AOOResendData` | Receiver -> Sender | Request retransmission of missing packets |
| `AOOPingSink` | Sender -> Receiver | Ping (keep-alive + clock sync) |
| `AOOPingSource` | Receiver -> Sender | Ping in the reverse direction |
| `AOOPongSink` | Sender -> Receiver | Pong reply with three NTP-style timestamps |
| `AOOPongSource` | Receiver -> Sender | Pong reply in the reverse direction |
| `AOOInvite` | Receiver -> Sender | Receiver requests a sender to start streaming to it |
| `AOOUninvite` | Receiver -> Sender | Receiver asks a sender to stop streaming |
| `AOODecline` | Sender -> Receiver | Sender refuses a receiver's invitation |

### Multi-Receiver Addressing

A single `AOOSender` can send to multiple receivers simultaneously. Each target is identified by its AOO ID and an optional IP address + port:

```cpp
cfg.sink_targets = {
  {1, IPAddress(192, 168, 1, 10), 7000},  // receiver 1 at specific IP
  {2, IPAddress(192, 168, 1, 11), 7000},  // receiver 2 at different IP
};
```

When a target has a non-zero IP, the sender retargets the stream before each send using `AOOStream::setRemote()`. This uses a single socket for all destinations. When `sink_targets` is empty, messages are sent to the address configured in the `AOOStreamUDP` constructor.

Receivers that send an `AOOInvite` are automatically added with their IP resolved from the incoming packet.

### Invite / Uninvite

Demand-driven streaming: a receiver can request a sender to start or stop sending.

- **`AOOReceiver::invite(source_id)`** sends an invite. The sender accepts by adding the receiver to its targets and sending a start message, or declines if it's not active.
- **`AOOReceiver::uninvite(source_id)`** asks the sender to stop. The sender sends a stop message and removes the receiver from its targets.

### Redundancy

Each audio block can be sent multiple times to tolerate packet loss without waiting for retransmission. Set `redundancy` in `AOOSenderConfig`:

```cpp
cfg.redundancy = 2;  // send each block twice
```

The receiver silently deduplicates via sequence number. This is especially useful on lossy WiFi where the latency cost of a resend request may be too high.

### Multi-Frame Splitting and Reassembly

When encoded audio exceeds `max_frame_size`, the sender automatically splits it across multiple OSC messages sharing the same sequence number. The receiver reassembles them before decoding. This keeps individual UDP packets below the MTU.

### Jitter Buffer

`AOOJitterBuffer` reorders incoming blocks by sequence number and absorbs network timing variations. Blocks are held for a configurable depth before being released in strict order. Gaps that aren't filled in time are replaced with silence. Slot memory is allocated dynamically based on actual block sizes — no upfront size configuration needed. Enable it by setting `jitter_buffer_depth` in `AOOReceiverConfig`.

### Adaptive Resampler and Drift Estimation

`AOOResampler` wraps the AudioTools `ResampleStream` and continuously adjusts the playback rate to compensate for crystal-oscillator drift between sender and receiver.

The **primary** drift signal comes from `AOODriftEstimator`, which measures the effective source sample rate directly from the timestamps carried in each `AOOData` message. By comparing elapsed source time vs. elapsed local time between successive blocks, it derives a smoothed drift ratio via an exponential moving average.

A **secondary** buffer-level correction nudges the ratio to keep the jitter buffer near its target fill level, preventing long-term buffer underrun or overflow.

The combined ratio is clamped to +/-5% to avoid audible pitch artifacts. Enable it by setting `adaptive_resampling = true` in `AOOReceiverConfig`.

### Clock Synchronization

`AOOClockSync` estimates the wall-clock offset and round-trip time between sender and receiver using NTP-style calculations from ping/pong timestamps (t1, t2, t3, t4). The offset is smoothed with an exponential moving average. Both `AOOSender` and `AOOReceiver` expose a `clockSync()` accessor. This is useful for latency monitoring and time-aligning multiple streams, but is **not** used for the resampler — the data-timestamp-based `AOODriftEstimator` provides a more accurate sample-clock signal for that purpose.

### Packet Recovery

`AOOPacketRecovery` tracks gaps in received sequence numbers and schedules resend requests. When a gap is detected, the missing sequences are recorded with a configurable wait time. After the wait, the receiver sends `AOOResendData` messages to the sender. The sender re-transmits the data from its buffer with the original sequence number to the specific receiver that requested it. Packets that exceed the maximum retry count are abandoned. This is enabled automatically; tune the timing via `recovery_wait_ms` and `recovery_max_requests` in `AOOReceiverConfig`.

### Codec Delay Compensation

When mixing multiple sources that use different codecs (e.g. Opus vs PCM), each codec introduces a different encoding/decoding latency. The sender communicates its `codec_delay_samples` in the `AOOStart` message. The receiver inserts silence equal to the codec delay at stream start, aligning all senders in the mix. Set the delay on the sender via `codec_delay_samples` in `AOOSenderConfig`.

### Codec Support

PCM is built-in. Additional codecs (e.g. Opus) can be registered:

```cpp
// Sender side
OpusAudioEncoder opus;
sender.setEncoder("opus", opus);

// Receiver side
receiver.addDecoder("opus", []() {
  return (AudioDecoder*)new OpusAudioDecoder();
});
```

### Transport

The library uses `AOOStream` as the transport abstraction. Two implementations are provided:

| Class | Description |
|-------|-------------|
| `AOOStreamUDP` | UDP transport. Each `write()` sends one UDP packet, providing natural OSC message framing. Supports unicast and multicast via a constructor flag. |
| `AOOStreamSerial` | Serial transport using HDLC framing (0x7E delimiters + byte-stuffing) to provide message boundaries over a raw byte stream (UART, USB-Serial). |

```cpp
// Unicast UDP
AOOStreamUDP udp(targetIP, 7000);

// Multicast UDP
AOOStreamUDP udp(multicastIP, 7000, true);

// Serial with HDLC framing
AOOStreamSerial serial(Serial1);
```

The stream's `begin()` is called automatically by `AOOSender::begin()` / `AOOReceiver::begin()`. Custom transports can subclass `AOOStream` directly.

## Installation in Arduino

You can download the libraries as zip and call include Library -> zip library. Or you can git clone this project into the Arduino libraries folder e.g. with

```
cd  ~/Documents/Arduino/libraries
git clone https://github.com/pschatzmann/arduino-audio-tools.git
git clone https://github.com/pschatzmann/arduino-aoo.git
```

I recommend to use git because you can easily update to the latest version just by executing the git pull command in the project folder. 
