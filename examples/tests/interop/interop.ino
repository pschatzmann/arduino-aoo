/**
 * @brief Interoperability test with the official aoo library over UDP on
 * localhost (desktop only). Configured by environment variables:
 * - AOO_MODE: "receive" (receive from an official source) or "send" (send
 *   to an official sink)
 * - AOO_PORT: local UDP port, AOO_REMOTE_PORT: port of the official peer
 * - AOO_SECONDS: duration
 * In receive mode the received 441 Hz sine is analyzed and the test fails
 * if it was not received correctly. In send mode a 441 Hz sine is sent; the
 * official sink reports what it received.
 */
#include "AOO.h"
#include "AudioTools.h"

const int BLOCK = 128;
const AudioInfo info(44100, 1, 16);

int envInt(const char *name, int def) {
  const char *value = getenv(name);
  return value != nullptr ? atoi(value) : def;
}

/// Sleeps until the next block is due (real time pacing)
void waitForBlock(uint64_t start, int block_no) {
  uint64_t due = start + (uint64_t)block_no * BLOCK * 1000000ULL / 44100;
  while (micros() < due) delayMicroseconds(200);
}

int receive(int port, int seconds) {
  AOOStreamUDP<WiFiUDP> udp(port);
  AOOReceiver receiver(1, udp);
  auto cfg = receiver.defaultConfig();
  cfg.copyFrom(info);
  if (!receiver.begin(cfg)) return 1;

  int16_t buf[BLOCK];
  double sum2 = 0;
  long n = 0, crossings = 0;
  int16_t last = 0;
  uint64_t start = micros();
  int blocks = seconds * 44100 / BLOCK;
  for (int b = 0; b < blocks; b++) {
    if (receiver.available() > 0) {
      size_t len = receiver.readBytes((uint8_t *)buf, sizeof(buf));
      for (size_t i = 0; i < len / 2; i++) {
        if (buf[i] == 0) continue;
        sum2 += (double)buf[i] * buf[i];
        n++;
        if ((last < 0) != (buf[i] < 0)) crossings++;
        last = buf[i];
      }
    }
    waitForBlock(start, b + 1);
  }
  double rms = n > 0 ? sqrt(sum2 / n) / 32768.0 : 0;
  double cps = n > 0 ? crossings * 44100.0 / n : 0;
  auto &st = receiver.stats();
  auto &ms = receiver.messageStats();
  printf(
      "arduino-aoo receiver: samples=%ld rms=%.3f crossings_per_sec=%.0f "
      "osc_start=%u binary_data=%u osc_data=%u stored=%u resend=%u "
      "start_requests=%u parse_errors=%u\n",
      n, rms, cps, ms.osc_start, ms.binary_data, ms.osc_data,
      st.segments_stored, st.resend_requests, st.start_requests,
      ms.parse_errors);
  // a 441 Hz sine with amplitude 0.5: rms 0.354, 882 zero crossings/sec
  bool ok = n > 44100 * (seconds - 2) * 0.98 && fabs(rms - 0.354) < 0.03 &&
            fabs(cps - 882) < 30;
  printf("%s\n", ok ? "PASSED" : "FAILED");
  return ok ? 0 : 1;
}

int send(int port, int remote_port, int seconds) {
  AOOStreamUDP<WiFiUDP> udp(IPAddress(127, 0, 0, 1), remote_port);
  udp.setLocalPort(port);
  AOOSender sender(1, udp);
  auto cfg = sender.defaultConfig();
  cfg.copyFrom(info);
  cfg.sink_targets.push_back(AOOSinkTarget(1));
  if (!sender.begin(cfg)) return 1;

  int16_t buf[BLOCK];
  double phase = 0;
  uint64_t start = micros();
  int blocks = seconds * 44100 / BLOCK;
  for (int b = 0; b < blocks; b++) {
    for (int i = 0; i < BLOCK; i++) {
      buf[i] = (int16_t)(0.5 * 32767 * sin(phase * 2 * M_PI));
      phase = fmod(phase + 441.0 / 44100, 1.0);
    }
    sender.write((const uint8_t *)buf, sizeof(buf));
    waitForBlock(start, b + 1);
  }
  sender.end();
  printf("arduino-aoo sender: sent %d blocks, ping rtt=%lu us\n", blocks,
         (unsigned long)sender.clockSync().rttMicros());
  return 0;
}

void setup() {
  AudioToolsLogger.begin(Serial, (AudioToolsLogLevel)envInt("AOO_LOG_LEVEL", (int)AudioToolsLogLevel::Warning));
  const char *mode = getenv("AOO_MODE");
  int port = envInt("AOO_PORT", 9998);
  int remote_port = envInt("AOO_REMOTE_PORT", 9999);
  int seconds = envInt("AOO_SECONDS", 3);
  int rc = 2;
  if (mode != nullptr && strcmp(mode, "receive") == 0) rc = receive(port, seconds);
  if (mode != nullptr && strcmp(mode, "send") == 0)
    rc = send(port, remote_port, seconds);
  fflush(stdout);
  exit(rc);
}

void loop() {}
