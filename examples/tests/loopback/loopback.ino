/**
 * @brief Loopback integration test: the sender encodes a PCM ramp into AOO
 * messages, which are delivered to the receiver via an in-memory,
 * packet-preserving pipe. The decoded audio must be identical to the input.
 */
#include "../test-support/AOOTestSupport.h"

// global objects: they are destroyed after setup() exits the program, which
// must not crash
Link aoo_link;
AOOSender sender(1, aoo_link.sender_io);
AOOReceiver receiver(1, aoo_link.receiver_io);

void testLoopback() {
  Serial.println("testLoopback");
  assert(receiver.begin(receiverConfig(receiver)));
  assert(sender.begin(senderConfig(sender)));

  // stream: the receiver starts to provide data once the buffer is primed
  std::vector<int16_t> out;
  for (int b = 0; b < 50; b++) {
    writeBlock(sender, b);
    if (b < 4) continue;
    auto block = pullSamples(receiver, BLOCK_SAMPLES);
    out.insert(out.end(), block.begin(), block.end());
  }

  Serial.print("Sink captured ");
  Serial.print((int)out.size());
  Serial.print(" samples, mismatches: ");
  Serial.println(rampMismatches(out));
  assert(out.size() == 46 * BLOCK_SAMPLES);
  assert(rampMismatches(out) == 0);
}

void setup() {
  Serial.begin(115200);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);
  testLoopback();
  Serial.println("Loopback test completed");
  finishTests(0);  // a failed assert() aborts with a non-zero status
}

void loop() {}
