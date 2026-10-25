# AoO - Audio Over OSC for Arduino

"Audio over OSC" aka AoO is a message-based real-time audio streaming system using Open Sound Control as its wire format. It sends audio from arbitrary senders to arbitrary receivers on demand.

This is a header-only C++ implementation using the Arduino Networking API for communication and the [Arduino Audio Tools](https://github.com/pschatzmann/arduino-audio-tools) library for audio processing. It is compatible with the [official AoO library](https://git.iem.at/aoo/aoo), so a microcontroller can also exchange audio with Pure Data, SuperCollider or any other application using it.

## Key Features

- **Pull-based AudioStream receiver** — decoded audio via `readBytes()`, integrates with `StreamCopy`
- **Multi-source mixing** — receive from multiple senders simultaneously via `InputMixer`
- **Codec support** — PCM built-in, Opus (or any AudioTools codec) via plug-in registration
- **Buffered with priming** — encoded data buffered per source; playback starts after buffer is half full, giving time for packet recovery
- **Automatic packet recovery** — detects gaps, requests resends, quality-gated to avoid flooding
- **Adaptive resampling** — compensates clock drift between sender and receiver
- **FreeRTOS task support** — `AOOReceiverTask` processes UDP on a separate core (ESP32)
- **Late joining** — a receiver which missed the start of a stream requests the format from the sender
- **Binary + OSC interop** — compatible with the official aoo library's binary data format
- **Multi-receiver addressing** — one sender can target multiple receivers with per-target IP
- **Invite/uninvite** — demand-driven streaming: receivers request senders to start/stop
- **Redundancy** — send each block multiple times for WiFi loss tolerance
- **Clock synchronization** — NTP-style offset estimation via ping/pong
- **PSRAM support** - Buffers are automatically allocated in PSRAM when PSRAM is activated

## Core Classes

| Class | Description |
|-------|-------------|
| `AOOSender` | Sends audio over the network. Implements `AudioOutput` — plug into `StreamCopy`. |
| `AOOReceiver` | Receives and decodes audio from one or more senders. Implements `AudioStream` — pull decoded audio via `readBytes()`. |
| `AOOReceiverTask` | Variant of `AOOReceiver` which receives the UDP messages in a separate task: on a dual-core ESP32 the reception is not blocked while `loop()` is busy. |

## Examples

A sender is used like any AudioTools output (e.g. with a `StreamCopy` from a microphone or a generator) and a receiver like any AudioTools source (e.g. with a `StreamCopy` to I2S). The receiver converts all senders to the output format defined in its configuration. See the [Quick Start](https://github.com/pschatzmann/arduino-aoo/wiki/Quick-Start) for the code and the [examples](https://github.com/pschatzmann/arduino-aoo/tree/main/examples) for the complete sketches:

| Example | Description |
|---------|-------------|
| [aoo-sender](https://github.com/pschatzmann/arduino-aoo/tree/main/examples/aoo-sender) | sends a sine wave with Opus |
| [aoo-receiver](https://github.com/pschatzmann/arduino-aoo/tree/main/examples/aoo-receiver) | receives PCM or Opus and prints it as CSV (replace the output with I2S to play it) |
| [aoo-receiver-task](https://github.com/pschatzmann/arduino-aoo/tree/main/examples/aoo-receiver-task) | the same with `AOOReceiverTask` |
| [tests](https://github.com/pschatzmann/arduino-aoo/tree/main/examples/tests) | automated tests, which also run on a microcontroller |

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
- **Error correction**: resend requests (default) work well with Opus and 20 ms frames; redundancy (`cfg.redundancy = 2` on the sender) is the most robust against random loss but doubles the bandwidth. See [Choosing the Error Correction](https://github.com/pschatzmann/arduino-aoo/wiki/Architecture#choosing-the-error-correction) for measurements.
- **Interoperability**: tested in both directions against the official aoo library (PCM and Opus, desktop and ESP32-S3 over WiFi).

## Command Line Tool

The desktop build provides the `aoo` command line tool to send and receive audio, e.g. to test a microcontroller or to exchange audio with the official aoo library:

```bash
aoo send --host 192.168.1.33 --codec opus --input sine:440   # or mic, a WAV file or - for stdin
aoo receive --output speaker                                 # or a WAV or CSV file or - for stdout
```

See [Desktop Build & Tests](https://github.com/pschatzmann/arduino-aoo/wiki/Desktop-Build#command-line-tool) for the options.

## Documentation

- [Quick Start & Examples](https://github.com/pschatzmann/arduino-aoo/wiki/Quick-Start)
- [Architecture & Features](https://github.com/pschatzmann/arduino-aoo/wiki/Architecture)
- [Desktop Build & Tests](https://github.com/pschatzmann/arduino-aoo/wiki/Desktop-Build)
- [Class Documentation](https://pschatzmann.github.io/arduino-aoo/group__main.html)
- [AoO V2.0 Protocol Specification](https://git.iem.at/aoo/aoo/-/blob/master/doc/aoo_protocol.md)
- [OSC V1.0 Specification](https://opensoundcontrol.stanford.edu/spec-1_0.html)

## Installation in Arduino

This library needs the [Arduino Audio Tools](https://github.com/pschatzmann/arduino-audio-tools) and, for the Opus codec, [arduino-libopus](https://github.com/pschatzmann/arduino-libopus).

### Installation with ZIP files

1. Download the ZIP files of the libraries from GitHub (the green **Code** button -> **Download ZIP**), or directly:
   - [arduino-audio-tools](https://github.com/pschatzmann/arduino-audio-tools/archive/refs/heads/main.zip)
   - [arduino-aoo](https://github.com/pschatzmann/arduino-aoo/archive/refs/heads/main.zip)
   - optional: [arduino-libopus](https://github.com/pschatzmann/arduino-libopus/archive/refs/heads/main.zip)
2. In the Arduino IDE select **Sketch -> Include Library -> Add .ZIP Library...** and select each downloaded ZIP file (don't unpack them).
3. Restart the Arduino IDE: the examples are then available in **File -> Examples -> Audio Over OSC**.

To update a library, delete its folder (e.g. `arduino-aoo-main`) in the `libraries` folder of your sketchbook and add the new ZIP file again.

### Installation with Git

Alternatively clone the libraries into your Arduino libraries folder: this makes the updates easier (`git pull`):

```bash
cd ~/Documents/Arduino/libraries
git clone https://github.com/pschatzmann/arduino-audio-tools.git
git clone https://github.com/pschatzmann/arduino-aoo.git
# optional: for the Opus codec
git clone https://github.com/pschatzmann/arduino-libopus.git
```
