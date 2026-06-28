# Building on the Desktop

This project can be compiled and run on Linux and macOS using CMake. The build uses the [arduino-audio-tools](https://github.com/pschatzmann/arduino-audio-tools) Arduino emulator to run Arduino sketches as native desktop executables.

## Prerequisites

- CMake 3.22+
- A C++ compiler with C++11 support (GCC or Clang)
- Git (dependencies are fetched automatically)

On Linux (Debian/Ubuntu):

```bash
sudo apt install cmake g++ git
```

On macOS:

```bash
brew install cmake
```

## Build

```bash
mkdir build
cd build
cmake ..
make
```

This fetches all dependencies automatically via CMake `FetchContent`:

- **arduino-audio-tools** — audio processing library and Arduino emulator
- **arduino-libopus** — Opus codec (for the sender and receiver examples)
- **miniaudio.h** — audio I/O for the receiver example (downloaded at configure time)

## Build Targets

The build produces the following executables:

| Target | Location | Description |
|--------|----------|-------------|
| `aoo-sender` | `examples/aoo-sender/` | Sends audio over UDP with Opus encoding |
| `aoo-receiver` | `examples/aoo-receiver/` | Receives and plays audio using miniaudio |
| `protocol` | `examples/tests/protocols/` | Protocol message tests |
| `log-messages` | `examples/tests/log-messages/` | Logging tests |
| `loopback` | `examples/tests/loopback/` | Loopback tests |

Build a single target:

```bash
make aoo-sender
```

## Preprocessor Defines

Desktop builds automatically define:

- `ARDUINO` — enables Arduino API compatibility
- `IS_DESKTOP` — signals desktop environment (for platform-specific code paths)
- `DEFINE_MAIN` — the Arduino emulator provides `main()`

## Running

After building, run the examples from the build directory:

```bash
./examples/aoo-sender/aoo-sender
./examples/aoo-receiver/aoo-receiver
```

The sender and receiver communicate over UDP and are compatible with the [official AoO library](https://git.iem.at/aoo/aoo).
