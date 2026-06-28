# AoO - Audio Over OSC

"Audio over OSC" aka AoO is a message-based real-time audio streaming system using Open Sound Control as its wire format. It sends audio from arbitrary senders to arbitrary receivers on demand.

This is a header-only C++ implementation using the Arduino Networking API for communication and the [Arduino Audio Tools](https://github.com/pschatzmann/arduino-audio-tools) library for audio processing. It is compatible with the [official AoO library](https://git.iem.at/aoo/aoo).

## Key Features

- **Pull-based AudioStream receiver** — decoded audio via `readBytes()`, integrates with `StreamCopy`
- **Multi-source mixing** — receive from multiple senders simultaneously via `InputMixer`
- **Codec support** — PCM built-in, Opus (or any AudioTools codec) via plug-in registration
- **Buffered with priming** — encoded data buffered per source; playback starts after buffer is half full, giving time for packet recovery
- **Automatic packet recovery** — detects gaps, requests resends, quality-gated to avoid flooding
- **Adaptive resampling** — compensates clock drift between sender and receiver
- **FreeRTOS task support** — `AOOReceiverTask` processes UDP on a separate core (ESP32)
- **Binary + OSC interop** — compatible with the official aoo library's binary data format
- **Multi-receiver addressing** — one sender can target multiple receivers with per-target IP
- **Invite/uninvite** — demand-driven streaming: receivers request senders to start/stop
- **Redundancy** — send each block multiple times for WiFi loss tolerance
- **Clock synchronization** — NTP-style offset estimation via ping/pong

## Core Classes

| Class | Description |
|-------|-------------|
| `AOOSender` | Sends audio over the network. Implements `AudioOutput` — plug into `StreamCopy`. |
| `AOOReceiver` | Receives and decodes audio from one or more senders. Implements `AudioStream` — pull decoded audio via `readBytes()`. |
| `AOOReceiverTask` | ESP32 variant that processes UDP in a separate FreeRTOS task for zero packet loss. |

## Transport

| Class | Description |
|-------|-------------|
| `AOOStreamUDP` | UDP transport (unicast or multicast) |
| `AOOStreamSerial` | Serial transport with HDLC framing |

## Platforms

### What works well

| Platform | Sending | Receiving | Notes |
|----------|---------|-----------|-------|
| **ESP32** (dual-core) | ✅ PCM + Opus | ✅ PCM + Opus | Best platform. Use `AOOReceiverTask` for dedicated UDP core. WiFi throughput is the main constraint. |
| **ESP32-S3** | ✅ PCM + Opus | ✅ PCM + Opus | Dual-core, more RAM than ESP32. Same WiFi considerations. |
| **Desktop** (Linux/Mac) | ✅ PCM + Opus | ✅ PCM + Opus | Full interop with the official aoo library. Use for testing with `MiniAudioStream`. |

### Challenges

| Platform | Sending | Receiving | Notes |
|----------|---------|-----------|-------|
| **ESP32-S2/C3** (single-core) | ✅ PCM, ⚠️ Opus | ⚠️ PCM, ❌ Opus | No second core for `AOOReceiverTask`. Receiving competes with audio output for CPU. Opus decoding is too heavy for single-core at higher sample rates. |
| **ESP8266** | ⚠️ PCM only | ⚠️ PCM only | Limited RAM (~40KB heap). No Opus support. WiFi stack is less reliable for real-time. No FreeRTOS task support. |
| **RP2040** (Pico W) | ⚠️ PCM + Opus | ⚠️ PCM + Opus | WiFi throughput is limited. FreeRTOS available but not tested. Opus works but CPU is tight at higher sample rates. |
| **Arduino (AVR/SAMD)** | ❌ | ❌ | Insufficient RAM and CPU for real-time audio streaming. |

### Key considerations

- **WiFi bandwidth**: PCM mono at 44.1kHz = 88 KB/s, stereo = 176 KB/s. Opus reduces this to ~10-20 KB/s. WiFi can handle both, but packet timing jitter causes gaps at higher PCM rates.
- **Opus encoding/decoding**: Requires ~50-100KB of RAM, ~30KB of stack, and significant CPU. Set `complexity = 1` on ESP32 to reduce CPU load. Increase the task/loop stack size (e.g. `SET_LOOP_TASK_STACK_SIZE(30000)`) when using Opus. Mono works better than stereo on constrained platforms.
- **Sender is lightweight**: Encoding + UDP send is straightforward. Even single-core platforms handle PCM sending well.
- **Receiver is heavier**: Decoding + format conversion + mixing + audio output. On single-core platforms, use larger block sizes and lower sample rates to reduce per-block overhead.
- **Serial transport** (`AOOStreamSerial`): Works on any platform with UART. Useful for point-to-point links where WiFi isn't available. Limited by baud rate (~11 KB/s at 115200).

## Documentation

- [Quick Start & Examples](doc/quickstart.md)
- [Architecture & Features](doc/architecture.md)
- [AoO V2.0 Protocol Specification](https://git.iem.at/aoo/aoo/-/blob/master/doc/aoo_protocol.md)
- [OSC V1.0 Specification](https://opensoundcontrol.stanford.edu/spec-1_0.html)

## Installation in Arduino

Clone the libraries into your Arduino libraries folder: 

```bash
cd ~/Documents/Arduino/libraries
git clone https://github.com/pschatzmann/arduino-audio-tools.git
git clone https://github.com/pschatzmann/arduino-aoo.git
```

Or download as ZIP and use Arduino IDE: Sketch -> Include Library -> Add .ZIP Library.
