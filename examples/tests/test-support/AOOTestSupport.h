#pragma once
/**
 * @brief Shared helpers for the automated AOO tests: a minimal test harness
 * and an in-memory, packet-preserving AOOStream so that sender and receiver
 * can talk to each other without a network.
 */
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

#include "AOO.h"
#include "AudioTools.h"

using namespace arduino_aoo;

// ------------------------------------------------------------------
// Minimal test harness
// ------------------------------------------------------------------

static int aoo_test_failures = 0;

#define CHECK(cond)                                                   \
  do {                                                                \
    if (!(cond)) {                                                    \
      aoo_test_failures++;                                            \
      printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
    }                                                                 \
  } while (0)

#define CHECK_EQ(a, b)                                                \
  do {                                                                \
    long long va_ = (long long)(a), vb_ = (long long)(b);             \
    if (va_ != vb_) {                                                 \
      aoo_test_failures++;                                            \
      printf("    FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__,   \
             __LINE__, #a, #b, va_, vb_);                             \
    }                                                                 \
  } while (0)

struct AOOTestCase {
  const char *name;
  void (*fn)();
};

/// Runs all tests, prints a summary and returns the number of failed tests.
/// On the desktop the environment variable AOO_TEST_FILTER limits the run to
/// the tests whose name contains its value.
inline int runTests(const AOOTestCase *tests, int count) {
  const char *filter = nullptr;
#if defined(IS_DESKTOP)
  filter = getenv("AOO_TEST_FILTER");
#endif
  int failed = 0, executed = 0;
  for (int i = 0; i < count; i++) {
    if (filter != nullptr && strstr(tests[i].name, filter) == nullptr)
      continue;
    executed++;
    int before = aoo_test_failures;
    printf("[ RUN  ] %s\n", tests[i].name);
    tests[i].fn();
    bool ok = aoo_test_failures == before;
    if (!ok) failed++;
    printf("[ %s ] %s\n", ok ? " OK " : "FAIL", tests[i].name);
  }
  printf("\n%d of %d tests passed\n", executed - failed, executed);
  return failed;
}

/// Ends the test executable with a status usable by ctest
inline void finishTests(int failed) {
  fflush(stdout);
#if defined(IS_DESKTOP)
  exit(failed == 0 ? 0 : 1);
#else
  while (true) delay(1000);
#endif
}

// ------------------------------------------------------------------
// In-memory transport
// ------------------------------------------------------------------

/// A queue of datagrams
using AOOPacketQueue = std::deque<std::vector<uint8_t>>;

/// Guards the queues when a receiver runs in its own task
inline std::recursive_mutex &aooPipeMutex() {
  static std::recursive_mutex mutex;
  return mutex;
}

/**
 * @brief AOOStream that reads datagrams from one queue and writes each
 * write() as a separate datagram to one or more output queues. Unlike a
 * byte-oriented QueueStream this preserves message boundaries, like UDP.
 * An optional drop filter can be used to simulate packet loss.
 */
class AOOStreamPipe : public AOOStream {
 public:
  /// @param in queue with the incoming datagrams
  /// @param peerIpLastByte defines senderIP() reported for incoming datagrams
  AOOStreamPipe(AOOPacketQueue &in, uint8_t peerIpLastByte = 1)
      : peer_ip(127, 0, 0, peerIpLastByte), p_in(&in) {}

  /// Address reported as sender of the incoming datagrams
  IPAddress peer_ip;

  /// Adds a queue that receives everything written to this stream
  void addOutput(AOOPacketQueue &out) { outputs.push_back(&out); }

  /// Return true to drop the packet (simulates loss)
  std::function<bool(const uint8_t *, size_t)> drop_filter;

  int sent = 0;
  int dropped = 0;

  int available() override {
    std::lock_guard<std::recursive_mutex> lock(aooPipeMutex());
    if (pos >= current.size() && !p_in->empty()) {
      current = std::move(p_in->front());
      p_in->pop_front();
      pos = 0;
    }
    return current.size() - pos;
  }
  int read() override { return available() > 0 ? current[pos++] : -1; }

  /// Number of readBytes() calls (bulk reads of a datagram)
  int bulk_reads = 0;
  size_t readBytes(uint8_t *data, size_t len) override {
    bulk_reads++;
    size_t n = min((size_t)available(), len);
    memcpy(data, current.data() + pos, n);
    pos += n;
    return n;
  }
  int peek() override { return available() > 0 ? current[pos] : -1; }

  size_t write(const uint8_t *data, size_t len) override {
    std::lock_guard<std::recursive_mutex> lock(aooPipeMutex());
    if (drop_filter && drop_filter(data, len)) {
      dropped++;
      return len;
    }
    sent++;
    sent_to.push_back((uint32_t)remote_ip);
    for (auto *out : outputs) out->emplace_back(data, data + len);
    return len;
  }
  size_t write(uint8_t val) override { return write(&val, 1); }

  /// Remote address per sent packet (records retargeting by the sender)
  std::vector<uint32_t> sent_to;

  void setRemote(IPAddress ip, uint16_t port) override { remote_ip = ip; }
  IPAddress remoteIP() override { return remote_ip; }
  IPAddress senderIP() override { return peer_ip; }
  uint16_t senderPort() override { return 9000; }

 protected:
  AOOPacketQueue *p_in;
  std::vector<AOOPacketQueue *> outputs;
  IPAddress remote_ip;
  std::vector<uint8_t> current;
  size_t pos = 0;
};

/// Number of queued datagrams whose OSC address contains the text
inline int aooCountAddress(const AOOPacketQueue &queue, const char *text) {
  int count = 0;
  for (auto &p : queue) {
    if (p.size() > 0 && p[0] == '/' &&
        strstr((const char *)p.data(), text) != nullptr)
      count++;
  }
  return count;
}

/// True if the datagram is an OSC message whose address contains the text
inline bool aooAddressContains(const uint8_t *data, size_t len,
                               const char *text) {
  if (len == 0 || data[0] != '/') return false;
  return strstr((const char *)data, text) != nullptr;
}

/**
 * @brief AOOReceiver which gives the tests access to the message processing
 * and post processing, so that protocol behavior can be tested without
 * pulling audio.
 */
class AOOTestReceiver : public AOOReceiver {
 public:
  using AOOReceiver::AOOReceiver;
  AOOTestReceiver() = default;
  /// Processes all queued messages
  void process() { processMessages(); }
  /// Resend requests, statistics and source management
  void postProcess() { postProcessing(); }
  /// Sample rate used by the adaptive resampling for the indicated source
  int resampleRate(int idx = 0) {
    return idx < (int)sources.size() ? sources[idx]->resampleRate() : 0;
  }
};

/// Parses the last /start message in the queue; returns false if none
inline bool aooFindStart(const AOOPacketQueue &queue, AOOStart &start) {
  bool found = false;
  for (auto &p : queue) {
    if (aooAddressContains(p.data(), p.size(), "/start") &&
        start.parse((uint8_t *)p.data(), p.size()))
      found = true;
  }
  return found;
}

/// Reads the seq number from an OSC /data message sent by AOOSender
inline int32_t aooDataSeq(const uint8_t *data, size_t len) {
  AOOData msg;
  if (!msg.parse((uint8_t *)data, len)) return -1;
  return msg.seq_no;
}

/// Generates a deterministic int16 ramp for a block
inline void aooFillBlock(int16_t *samples, int count, int block_no) {
  for (int i = 0; i < count; i++) {
    samples[i] = (int16_t)(block_no * count + i);
  }
}

// ------------------------------------------------------------------
// Fixtures for sender -> receiver tests
// ------------------------------------------------------------------

const AudioInfo info(44100, 1, 16);
const int BLOCK_SAMPLES = 128;
const int BLOCK_BYTES = BLOCK_SAMPLES * 2;

/// Sender and receiver connected via in-memory queues
struct Link {
  AOOPacketQueue to_receiver, to_sender;
  AOOStreamPipe sender_io{to_sender, 1};
  AOOStreamPipe receiver_io{to_receiver, 2};
  Link() {
    sender_io.addOutput(to_receiver);
    receiver_io.addOutput(to_sender);
  }
};

AOOSenderConfig senderConfig(AOOSender &sender, int sink_id = 1) {
  auto cfg = sender.defaultConfig();
  cfg.copyFrom(info);
  cfg.sink_targets.push_back(AOOSinkTarget(sink_id));
  return cfg;
}

AOOReceiverConfig receiverConfig(AOOReceiver &receiver) {
  auto cfg = receiver.defaultConfig();
  cfg.copyFrom(info);
  cfg.buffer_depth = 8;
  cfg.adaptive_resampling = false;
  return cfg;
}

void writeBlock(AOOSender &sender, int block_no) {
  int16_t block[BLOCK_SAMPLES];
  aooFillBlock(block, BLOCK_SAMPLES, block_no);
  sender.write((const uint8_t *)block, BLOCK_BYTES);
}

/// Pulls up to len bytes from the receiver like StreamCopy does
size_t pull(AOOReceiver &receiver, int16_t *out, size_t len) {
  if (receiver.available() <= 0) return 0;
  return receiver.readBytes((uint8_t *)out, len);
}

/// Pulls the indicated number of samples (in several reads if necessary)
std::vector<int16_t> pullSamples(AOOReceiver &receiver, int samples) {
  std::vector<int16_t> out(samples);
  size_t total = 0, bytes = samples * 2;
  for (int j = 0; j < 20 && total < bytes; j++) {
    if (receiver.available() <= 0) continue;
    total += receiver.readBytes((uint8_t *)out.data() + total, bytes - total);
  }
  out.resize(total / 2);
  return out;
}

/// Counts the samples that differ from the ramp created by aooFillBlock()
int rampMismatches(const std::vector<int16_t> &samples) {
  int mismatches = 0;
  for (size_t i = 0; i < samples.size(); i++) {
    if (samples[i] != (int16_t)i) mismatches++;
  }
  return mismatches;
}
