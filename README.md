# AoO - Audio Over OSC

"Audio over OSC" aka AoO is aimed to be a message based audio system inspired by Open Sound Control OSC as a syntax format. It is dedicated to send audio in real time from arbitrary sources to arbitrary sinks on demand.

This is a header-only C++ implementation that is using the Arduino Networking API for communications and the functionality of the Arduino Audio Tools for the audio processing.

#### Documentation

- [AoO V2.0 Protocol](https://git.iem.at/aoo/aoo/-/blob/cmake_update/doc/aoo_protocol.md) 
- [OSC V1.0 Specification](https://opensoundcontrol.stanford.edu/spec-1_0.html)


## Features

### Core Classes

| Class | Description |
|-------|-------------|
| `AOOSource` | Sends audio over the network. Encodes PCM (or any codec), splits into OSC messages, handles ping/pong keep-alive and resend requests from sinks. Implements `AudioOutput` so it plugs directly into the AudioTools `StreamCopy` pipeline. |
| `AOOSink` | Receives audio from one or more sources, decodes it, mixes multiple streams, and writes to an audio output. Call `copy()` in your `loop()` to process incoming messages. |
| `AOOSinkSingle` | Simplified single-source variant of `AOOSink` with lower memory usage. Buffers decoded blocks and copies them to the output via `StreamCopy`. |

### Configuration

Both source and sink accept a typed configuration struct that extends `AudioInfo`:

```cpp
AOOSource source(1, udp, 100);
auto cfg = source.defaultConfig();
cfg.sample_rate = 44100;
cfg.channels = 2;
cfg.bits_per_sample = 16;
cfg.sink_targets = {{2}};
cfg.redundancy = 2;
source.begin(cfg);
```

| Struct | Key fields |
|--------|-----------|
| `AOOSourceConfig` | `id`, `sink_targets`, `buffer_time_ms`, `max_frame_size`, `redundancy`, `codec_delay_samples`, `length_prefix`, `log_osc` |
| `AOOSinkConfig` | `id`, `jitter_buffer_depth`, `adaptive_resampling`, `recovery_wait_ms`, `recovery_max_requests`, `mixer_size`, `stream_timeout_ms`, `length_prefix`, `log_osc` |

The plain `begin(AudioInfo)` and `begin()` overloads still work for simple setups.

### Protocol Messages

All AOO v2.0 message types are implemented as structs that can serialize to and parse from OSC binary data:

| Message | Direction | Purpose |
|---------|-----------|---------|
| `AOOStart` | Source -> Sink | Start a stream (codec, sample rate, channels, block size) |
| `AOORequestStart` | Sink -> Source | Request the source to (re-)send the start message |
| `AOOStopSink` | Source -> Sink | Stop a stream |
| `AOOStopSource` | Sink -> Source | Request the source to stop |
| `AOOData` | Source -> Sink | Audio data with sequence number, timestamp, multi-frame support |
| `AOOResendData` | Sink -> Source | Request retransmission of missing packets |
| `AOOPingSink` | Source -> Sink | Ping (keep-alive + clock sync) |
| `AOOPingSource` | Sink -> Source | Ping in the reverse direction |
| `AOOPongSink` | Source -> Sink | Pong reply with three NTP-style timestamps |
| `AOOPongSource` | Sink -> Source | Pong reply in the reverse direction |
| `AOOInvite` | Sink -> Source | Sink requests a source to start streaming to it |
| `AOOUninvite` | Sink -> Source | Sink asks a source to stop streaming |
| `AOODecline` | Source -> Sink | Source refuses a sink's invitation |

### Multi-Sink and Per-Sink Addressing

A single `AOOSource` can send to multiple sinks simultaneously. Each sink target is identified by its AOO ID and an optional IP address + port:

```cpp
cfg.sink_targets = {
  {1, IPAddress(192, 168, 1, 10), 7000},  // sink 1 at specific IP
  {2, IPAddress(192, 168, 1, 11), 7000},  // sink 2 at different IP
};
```

When a target has a non-zero IP, the source retargets the UDP socket before each send using `UDPStream::setTarget()`. This uses a single socket for all destinations. When `sink_targets` is empty, messages are broadcast to the address set in `UDPStream::begin()`.

Sinks that send an `AOOInvite` are automatically added with their IP resolved from the incoming packet.

### Invite / Uninvite

Demand-driven streaming: a sink can request a source to start or stop sending.

- **`AOOSink::invite(source_id)`** sends an invite. The source accepts by adding the sink to its targets and sending a start message, or declines if it's not active.
- **`AOOSink::uninvite(source_id)`** asks the source to stop. The source sends a stop message and removes the sink from its targets.

### Redundancy

Each audio block can be sent multiple times to tolerate packet loss without waiting for retransmission. Set `redundancy` in `AOOSourceConfig`:

```cpp
cfg.redundancy = 2;  // send each block twice
```

The sink silently deduplicates via sequence number. This is especially useful on lossy WiFi where the latency cost of a resend request may be too high.

### Multi-Frame Splitting and Reassembly

When encoded audio exceeds `max_frame_size`, the source automatically splits it across multiple OSC messages sharing the same sequence number. The sink reassembles them before decoding. This keeps individual UDP packets below the MTU.

### Jitter Buffer

`AOOJitterBuffer` reorders incoming blocks by sequence number and absorbs network timing variations. Blocks are held for a configurable depth before being released in strict order. Gaps that aren't filled in time are replaced with silence. Slot memory is allocated dynamically based on actual block sizes — no upfront size configuration needed. Enable it by setting `jitter_buffer_depth` in `AOOSinkConfig`.

### Adaptive Resampler and Drift Estimation

`AOOResampler` wraps the AudioTools `ResampleStream` and continuously adjusts the playback rate to compensate for crystal-oscillator drift between source and sink.

The **primary** drift signal comes from `AOODriftEstimator`, which measures the effective source sample rate directly from the timestamps carried in each `AOOData` message. By comparing elapsed source time vs. elapsed local time between successive blocks, it derives a smoothed drift ratio via an exponential moving average.

A **secondary** buffer-level correction nudges the ratio to keep the jitter buffer near its target fill level, preventing long-term buffer underrun or overflow.

The combined ratio is clamped to +/-5% to avoid audible pitch artifacts. Enable it by setting `adaptive_resampling = true` in `AOOSinkConfig`.

### Clock Synchronization

`AOOClockSync` estimates the wall-clock offset and round-trip time between source and sink using NTP-style calculations from ping/pong timestamps (t1, t2, t3, t4). The offset is smoothed with an exponential moving average. Both `AOOSource` and `AOOSink` expose a `clockSync()` accessor. This is useful for latency monitoring and time-aligning multiple streams, but is **not** used for the resampler — the data-timestamp-based `AOODriftEstimator` provides a more accurate sample-clock signal for that purpose.

### Packet Recovery

`AOOPacketRecovery` tracks gaps in received sequence numbers and schedules resend requests. When a gap is detected, the missing sequences are recorded with a configurable wait time. After the wait, the sink sends `AOOResendData` messages to the source. The source re-transmits the data from its buffer with the original sequence number to the specific sink that requested it. Packets that exceed the maximum retry count are abandoned. This is enabled automatically; tune the timing via `recovery_wait_ms` and `recovery_max_requests` in `AOOSinkConfig`.

### Codec Delay Compensation

When mixing multiple sources that use different codecs (e.g. Opus vs PCM), each codec introduces a different encoding/decoding latency. The source communicates its `codec_delay_samples` in the `AOOStart` message. The sink inserts silence equal to the codec delay at stream start, aligning all sources in the mix. Set the delay on the source via `codec_delay_samples` in `AOOSourceConfig`.

### Codec Support

PCM is built-in. Additional codecs (e.g. Opus) can be registered:

```cpp
// Source side
OpusAudioEncoder opus;
source.setEncoder("opus", opus);

// Sink side
sink.addDecoder("opus", []() {
  return (AudioDecoder*)new OpusAudioDecoder();
});
```

### Transport

The library uses Arduino's `Stream` interface for network I/O, so any transport that implements `Stream` works: `UDPStream`, TCP, serial, WebSocket, etc. For non-packet-oriented transports (e.g. TCP), set `length_prefix = true` in the config to frame individual OSC messages with a uint64 size prefix.

## Installation in Arduino

You can download the libraries as zip and call include Library -> zip library. Or you can git clone this project into the Arduino libraries folder e.g. with

```
cd  ~/Documents/Arduino/libraries
git clone https://github.com/pschatzmann/arduino-audio-tools.git
git clone https://github.com/pschatzmann/arduino-aoo.git
```

I recommend to use git because you can easily update to the latest version just by executing the git pull command in the project folder. 
