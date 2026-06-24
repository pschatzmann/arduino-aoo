/**
 * @brief Loopback integration test: source encodes PCM into OSC,
 * sink decodes back. Uses separate streams for source output and
 * sink input, with a manual copy step in between so that the
 * source's receive() never consumes the sink's data.
 */

#include "AudioTools.h"
#include "AOO.h"

/// AOOStream wrapper around any Stream for non-network use (e.g. testing)
class AOOStreamQueue : public AOOStream {
 public:
  AOOStreamQueue(Stream &s) : p_stream(&s) {}
  int available() override { return p_stream->available(); }
  int read() override { return p_stream->read(); }
  int peek() override { return p_stream->peek(); }
  size_t readBytes(uint8_t *buf, size_t len) {
    return p_stream->readBytes(buf, len);
  }
  size_t write(const uint8_t *data, size_t len) override {
    return p_stream->write(data, len);
  }
  size_t write(uint8_t val) override { return p_stream->write(val); }
 protected:
  Stream *p_stream;
};

// Source writes OSC messages here
SingleBuffer<uint8_t> src_buf{1024 * 16};
QueueStream<uint8_t> src_queue{src_buf};
AOOStreamQueue src_stream{src_queue};

// Sink reads OSC messages from here
SingleBuffer<uint8_t> sink_buf{1024 * 16};
QueueStream<uint8_t> sink_queue{sink_buf};
AOOStreamQueue sink_stream{sink_queue};

// Sink decoded audio output
SingleBuffer<uint8_t> capture_buf{1024 * 8};
QueueStream<uint8_t> capture_stream{capture_buf};

AOOSource aoo_source(1, src_stream, 0);
AOOSink aoo_sink;

const int TEST_SAMPLES = 256;
const int BYTES = TEST_SAMPLES * 2;
int16_t test_data[TEST_SAMPLES];
int16_t expected_data[TEST_SAMPLES];

void generateTestData() {
  for (int i = 0; i < TEST_SAMPLES; i++) {
    test_data[i] = (int16_t)(i - 128);
    expected_data[i] = test_data[i];
  }
}

/// Copy all data from src_buf to sink_buf (simulates network delivery)
void deliverToSink() {
  int avail = src_buf.available();
  if (avail <= 0) return;
  uint8_t tmp[avail];
  int n = src_buf.readArray(tmp, avail);
  sink_buf.writeArray(tmp, n);
}

void testLoopback() {
  Serial.println("testLoopback");
  AudioInfo info(44100, 1, 16);

  src_buf.clear();
  sink_buf.clear();
  capture_buf.clear();
  src_queue.begin();
  sink_queue.begin();
  capture_stream.begin();

  // Source sends to src_stream with length prefix for message framing
  aoo_source.setStream(src_stream);
  auto src_cfg = aoo_source.defaultConfig();
  src_cfg.copyFrom(info);
  src_cfg.length_prefix = true;
  aoo_source.begin(src_cfg);

  // begin() wrote the start message to src_buf — deliver it to sink
  deliverToSink();

  // Source writes audio data (write() calls receive() which reads
  // from src_stream, but src_buf is empty now so it's a no-op)
  size_t written = aoo_source.write((const uint8_t *)test_data, BYTES);

  Serial.print("Source wrote ");
  Serial.print((int)written);
  Serial.print(" bytes, src_buf has ");
  Serial.print(src_buf.available());
  Serial.println(" bytes");

  assert(written > 0);

  // Deliver data messages to sink
  deliverToSink();
  assert(sink_buf.available() > 0);

  // Setup sink with length prefix to match source framing
  aoo_sink.setStream(sink_stream);
  aoo_sink.setOutput((Print &)capture_stream);
  auto sink_cfg = aoo_sink.defaultConfig();
  sink_cfg.copyFrom(info);
  sink_cfg.length_prefix = true;
  aoo_sink.begin(sink_cfg);

  // Process all messages
  aoo_sink.copy();

  int available = capture_buf.available();
  Serial.print("Sink captured ");
  Serial.print(available);
  Serial.println(" bytes");
  assert(available > 0);

  // Read back and verify the ramp pattern (PCM is lossless)
  int16_t out_data[TEST_SAMPLES];
  memset(out_data, 0, BYTES);
  int nbytes = available < BYTES ? available : BYTES;
  int rd = capture_buf.readArray((uint8_t *)out_data, nbytes);
  assert(rd > 0);

  int samples_read = rd / 2;
  int mismatches = 0;
  for (int i = 0; i < samples_read; i++) {
    if (out_data[i] != expected_data[i]) mismatches++;
  }
  Serial.print("Verified ");
  Serial.print(samples_read);
  Serial.print(" samples, mismatches: ");
  Serial.println(mismatches);
  assert(mismatches == 0);
}

void setup() {
  Serial.begin(115200);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);

  generateTestData();
  testLoopback();

  Serial.println("Loopback test completed");
}

void loop() {}
