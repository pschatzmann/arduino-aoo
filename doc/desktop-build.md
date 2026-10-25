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

If the arduino-audio-tools library is located next to this library (e.g. in the Arduino libraries folder), it is used, so that desktop and Arduino builds use the same version (disable with `-DAOO_USE_LOCAL_AUDIO_TOOLS=OFF`). A local Arduino Emulator can be used with `-DFETCHCONTENT_SOURCE_DIR_ARDUINO_EMULATOR=<path>`. All other dependencies are fetched automatically via CMake `FetchContent` (use `-DFETCHCONTENT_UPDATES_DISCONNECTED=OFF` to update a fetched copy):

- **arduino-audio-tools** — audio processing library and Arduino emulator
- **arduino-libopus** — Opus codec (for the sender and receiver examples)
- **miniaudio.h** — audio I/O for the receiver example (downloaded at configure time)

## Build Targets

The build produces the following executables:

| Target | Location | Description |
|--------|----------|-------------|
| `aoo-sender` | `examples/aoo-sender/` | Sends audio over UDP with Opus encoding |
| `aoo-receiver` | `examples/aoo-receiver/` | Receives audio and prints it as CSV |
| `aoo-receiver-task` | `examples/aoo-receiver-task/` | Same as aoo-receiver, using AOOReceiverTask (message processing on its own task/thread) |
| `protocol` | `examples/tests/protocols/` | Protocol message tests |
| `log-messages` | `examples/tests/log-messages/` | Logging tests |
| `loopback` | `examples/tests/loopback/` | Sender to receiver loopback test |
| `unit-tests` | `examples/tests/unit-tests/` | Automated unit and sender/receiver integration tests |
| `regression-tests` | `examples/tests/regression-tests/` | Automated tests for defects found in code reviews |
| `stream-test` | `examples/tests/stream-test/` | Sends or receives a sine over UDP with settings from environment variables, reports statistics |
| `interop` | `examples/tests/interop/` | Interoperability test with the official aoo library (see below) |

Build a single target:

```bash
make aoo-sender
```

## Automated Tests

The tests are registered with CTest. After building, run them from the build directory:

```bash
make protocol loopback unit-tests regression-tests
ctest --output-on-failure                # all tests
ctest -L unit                            # protocols, loopback and unit tests
ctest -L regression                      # regression tests only
AOO_TEST_FILTER=Multi ./examples/tests/regression-tests/regression-tests  # tests whose name contains "Multi"
```

`unit-tests` and `regression-tests` connect `AOOSender` and `AOOReceiver` through an in-memory, packet-preserving transport (`AOOStreamPipe` in `examples/tests/test-support/AOOTestSupport.h`), so no network or audio device is needed. Packet loss can be simulated with a drop filter. Each test executable returns a non-zero exit code when a test fails.

The same sketches can also be compiled with the Arduino IDE or arduino-cli. The results are printed to the serial console. `examples/tests/receiver-stats` analyzes a received sine on a microcontroller and prints statistics every 10 seconds.

### Interoperability with the official aoo library

With `-DAOO_BUILD_INTEROP_TESTS=ON` the official aoo library is fetched from git.iem.at (without Opus) and the `interop` test checks both directions over UDP on localhost: an official source sending to an arduino-aoo receiver and an arduino-aoo sender sending to an official sink. The duration can be changed with `AOO_INTEROP_SECONDS` (default 3):

```bash
cmake -DAOO_BUILD_INTEROP_TESTS=ON ..
make
ctest -L interop --output-on-failure
```

### Streaming tests

`stream-test` sends or receives a 440 Hz sine with the settings from environment variables (see the comment at the top of the sketch), e.g.:

```bash
AOO_MODE=receive AOO_PORT=9999 AOO_CODEC=opus AOO_CSV=out.csv ./examples/tests/stream-test/stream-test &
AOO_MODE=send AOO_CODEC=opus AOO_PORT=9998 AOO_REMOTE_PORT=9999 AOO_REDUNDANCY=2 ./examples/tests/stream-test/stream-test
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
