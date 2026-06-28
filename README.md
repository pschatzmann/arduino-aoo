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

## Documentation

- [Quick Start & Examples](doc/quickstart.md)
- [Architecture & Features](doc/architecture.md)
- [AoO V2.0 Protocol Specification](https://git.iem.at/aoo/aoo/-/blob/master/doc/aoo_protocol.md)
- [OSC V1.0 Specification](https://opensoundcontrol.stanford.edu/spec-1_0.html)

## Installation

Clone the libraries into your Arduino libraries folder: 

```bash
cd ~/Documents/Arduino/libraries
git clone https://github.com/pschatzmann/arduino-audio-tools.git
git clone https://github.com/pschatzmann/arduino-aoo.git
```

Or download as ZIP and use Arduino IDE: Sketch -> Include Library -> Add .ZIP Library.
