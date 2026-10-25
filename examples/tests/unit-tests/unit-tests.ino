/**
 * @brief Automated unit and integration tests for the AOO library.
 * Sender and receiver are connected with in-memory, packet-preserving
 * pipes, so no network is needed. On the desktop the executable exits with
 * 0 when all tests pass (see ctest), on a microcontroller the result is
 * printed to the console.
 */
#include "../test-support/AOOTestSupport.h"

// ------------------------------------------------------------------
// Buffer and helper classes
// ------------------------------------------------------------------

void testRingBufferIndexing() {
  IndexedRingBuffer<SingleBuffer<uint8_t>> buffer(4);
  uint8_t a[] = {1, 2, 3};
  uint8_t b[] = {9};
  CHECK_EQ(buffer.write(5, a, sizeof(a)), 3);
  CHECK(buffer.get(5) != nullptr);
  CHECK(buffer.get(1) == nullptr);  // same slot, different id
  CHECK_EQ(buffer.get(5)->available(), 3);
  // seq 9 maps to the same slot and replaces seq 5
  buffer.write(9, b, sizeof(b));
  CHECK(buffer.get(5) == nullptr);
  CHECK_EQ(buffer.get(9)->available(), 1);
  CHECK_EQ(buffer.get(9)->data()[0], 9);
}

void testBufferViewPrimingAndOrder() {
  IndexedRingBuffer<SingleBuffer<uint8_t>> buffer(4);
  IndexedRingBufferStreamView view;
  view.setBuffer(&buffer);
  view.setStartSeq(-1);
  uint8_t seg[3][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {8, 9, 10, 11}};
  uint8_t out[16] = {0};

  buffer.write(0, seg[0], 4);
  view.notifySegmentReceived(0);
  CHECK(!view.isPrimed());
  CHECK_EQ(view.readBytes(out, sizeof(out)), 0);

  // out of order arrival
  buffer.write(2, seg[2], 4);
  view.notifySegmentReceived(2);
  buffer.write(1, seg[1], 4);
  view.notifySegmentReceived(1);
  CHECK(view.isPrimed());

  CHECK_EQ(view.readBytes(out, 12), 12);
  for (int i = 0; i < 12; i++) CHECK_EQ(out[i], i);
}

void testBufferViewGapIsSilence() {
  IndexedRingBuffer<SingleBuffer<uint8_t>> buffer(4);
  IndexedRingBufferStreamView view;
  view.setBuffer(&buffer);
  view.setStartSeq(-1);
  uint8_t seg0[] = {1, 1, 1, 1};
  uint8_t seg2[] = {3, 3, 3, 3};
  buffer.write(0, seg0, 4);
  view.notifySegmentReceived(0);
  buffer.write(2, seg2, 4);
  view.notifySegmentReceived(2);

  uint8_t out[12];
  memset(out, 0xFF, sizeof(out));
  size_t total = 0;
  for (int j = 0; j < 4 && total < sizeof(out); j++) {
    total += view.readBytes(out + total, sizeof(out) - total);
  }
  CHECK_EQ(total, 12);
  for (int i = 0; i < 4; i++) CHECK_EQ(out[i], 1);
  for (int i = 4; i < 8; i++) CHECK_EQ(out[i], 0);  // missing seq 1
  for (int i = 8; i < 12; i++) CHECK_EQ(out[i], 3);
  CHECK_EQ(view.gapsFilled(), 1);
}

void testPacketRecoveryApi() {
  AOOPacketRecovery recovery;
  recovery.begin(10, 2);
  recovery.received(0);
  recovery.received(1);
  recovery.received(4);  // 2 and 3 missing
  CHECK_EQ(recovery.pendingCount(), 2);
  recovery.received(3);  // late arrival
  CHECK_EQ(recovery.pendingCount(), 1);

  int32_t seqs[8];
  CHECK_EQ(recovery.getResendRequests(seqs, 8), 0);  // not due yet
  delay(15);
  CHECK_EQ(recovery.getResendRequests(seqs, 8), 1);
  CHECK_EQ(seqs[0], 2);
  delay(15);
  CHECK_EQ(recovery.getResendRequests(seqs, 8), 1);  // 2nd attempt
  delay(15);
  CHECK_EQ(recovery.getResendRequests(seqs, 8), 0);  // abandoned
  CHECK_EQ(recovery.pendingCount(), 0);
}

void testNtpConversion() {
  uint64_t t = aoo_ntp_from_micros(3500000);  // 3.5 seconds
  CHECK_EQ(t >> 32, 3);
  CHECK_EQ(t & 0xFFFFFFFFULL, 0x80000000ULL);
  CHECK_EQ(aoo_ntp_diff_micros(aoo_ntp_from_micros(1250)), 1250);
  CHECK_EQ(aoo_ntp_diff_micros(-(int64_t)aoo_ntp_from_micros(1500000)),
           -1500000);
  uint64_t now = aoo_ntp_now();
  delay(20);
  int64_t elapsed = aoo_ntp_diff_micros(aoo_ntp_now() - now);
  CHECK(elapsed >= 19000 && elapsed < 200000);
}

/// Checks that the value is within +-1 (rounding of the NTP conversion)
#define CHECK_NEAR(a, b) CHECK((long long)(a) - (long long)(b) <= 1 && \
                               (long long)(b) - (long long)(a) <= 1)

void testClockSync() {
  // remote clock is 500us ahead, one way delay is 100us, 50us processing
  auto ntp = aoo_ntp_from_micros;
  AOOClockSync sync;
  CHECK(!sync.isValid());
  sync.update(ntp(1000), ntp(1600), ntp(1650), ntp(1250));
  CHECK(sync.isValid());
  CHECK_NEAR(sync.rttMicros(), 200);
  CHECK_NEAR(sync.offsetMicros(), 500);
  CHECK_NEAR(aoo_ntp_diff_micros(sync.toLocal(ntp(2500))), 2000);
  CHECK_NEAR(aoo_ntp_diff_micros(sync.toRemote(ntp(2000))), 2500);
}

// ------------------------------------------------------------------
// Sender
// ------------------------------------------------------------------

void testSenderEmitsStartAndData() {
  Link link;
  AOOSender sender(7, link.sender_io);
  CHECK(sender.begin(senderConfig(sender, 3)));
  // PCM: /start is sent with the first block, which defines the block size
  CHECK_EQ(link.to_receiver.size(), 0);
  writeBlock(sender, 0);
  writeBlock(sender, 1);
  CHECK_EQ(aooCountAddress(link.to_receiver, "/start"), 1);

  AOOStart start;
  CHECK(aooFindStart(link.to_receiver, start));
  CHECK_EQ(start.source_id, 7);
  CHECK_EQ(start.sink_id, 3);
  CHECK_EQ(start.sample_rate, 44100);
  CHECK_EQ(start.channels, 1);
  CHECK(strcmp(start.codec, "pcm") == 0);
  CHECK_EQ(start.block_size, BLOCK_SAMPLES);

  int data_msgs = 0;
  int32_t expected_seq = 0;
  for (auto &p : link.to_receiver) {
    if (!aooAddressContains(p.data(), p.size(), "/aoo/sink/3/data")) continue;
    AOOData data;
    CHECK(data.parse(p.data(), p.size()));
    CHECK_EQ(data.source_id, 7);
    CHECK_EQ(data.stream_id, start.stream_id);
    CHECK_EQ(data.seq_no, expected_seq++);
    CHECK_EQ(data.audio_data.len, BLOCK_BYTES);
    data_msgs++;
  }
  CHECK_EQ(data_msgs, 2);
  CHECK_EQ(sender.getTotalBytesSent(), 2 * BLOCK_BYTES);
}

void testSenderAnswersResendRequest() {
  Link link;
  AOOSender sender(1, link.sender_io);
  sender.begin(senderConfig(sender));
  for (int b = 0; b < 4; b++) writeBlock(sender, b);
  AOOData last;
  auto &pkt = link.to_receiver.back();
  CHECK(last.parse(pkt.data(), pkt.size()));
  link.to_receiver.clear();

  AOOResendData req;
  req.source_id = 1;
  req.sink_id = 1;
  req.stream_id = last.stream_id;
  req.resend_items.push_back({2, 0});
  req.send(link.receiver_io);
  sender.receive();

  int resent = 0;
  for (auto &p : link.to_receiver) {
    if (aooDataSeq(p.data(), p.size()) == 2) resent++;
  }
  CHECK_EQ(resent, 1);
}

// ------------------------------------------------------------------
// Sender -> Receiver
// ------------------------------------------------------------------

// The first available()/readBytes() calls are made only after the buffer is
// primed and the receiver reads less than it has buffered: see the
// regression tests for what happens otherwise.

void testPcmLoopbackDeliversBufferedAudio() {
  Link link;
  AOOSender sender(1, link.sender_io);
  AOOReceiver receiver(1, link.receiver_io);
  CHECK(receiver.begin(receiverConfig(receiver)));
  CHECK(sender.begin(senderConfig(sender)));
  for (int b = 0; b < 6; b++) writeBlock(sender, b);

  auto out = pullSamples(receiver, 4 * BLOCK_SAMPLES);
  CHECK_EQ(receiver.getSourceCount(), 1);
  CHECK_EQ(receiver.stats().segments_stored, 6);
  CHECK_EQ(out.size(), 4 * BLOCK_SAMPLES);
  CHECK_EQ(rampMismatches(out), 0);
}

void testMultiFrameReassembly() {
  Link link;
  AOOSender sender(1, link.sender_io);
  AOOReceiver receiver(1, link.receiver_io);
  receiver.begin(receiverConfig(receiver));
  auto cfg = senderConfig(sender);
  cfg.max_packet_size = 200;  // 256 byte blocks -> 3 frames
  sender.begin(cfg);
  for (int b = 0; b < 6; b++) writeBlock(sender, b);
  CHECK_EQ(aooCountAddress(link.to_receiver, "/data"), 6 * 3);

  // content is checked by the regression tests
  auto out = pullSamples(receiver, 4 * BLOCK_SAMPLES);
  CHECK_EQ(receiver.stats().assembly_completed, 6);
  CHECK_EQ(out.size(), 4 * BLOCK_SAMPLES);
}

void testRedundantSendsEachBlockTwice() {
  Link link;
  AOOSender sender(1, link.sender_io);
  auto cfg = senderConfig(sender);
  cfg.redundancy = 2;
  sender.begin(cfg);
  writeBlock(sender, 0);
  CHECK_EQ(aooCountAddress(link.to_receiver, "/data"), 2);
}

void testPingPongUpdatesClockSync() {
  Link link;
  AOOSender sender(1, link.sender_io);
  AOOTestReceiver receiver(1, link.receiver_io);
  receiver.begin(receiverConfig(receiver));
  sender.begin(senderConfig(sender));
  writeBlock(sender, 0);  // starts the stream: pings follow the /start
  sender.receive();       // sends the first ping
  receiver.process();     // answers with pong
  sender.receive();       // processes pong
  CHECK_EQ(receiver.messageStats().osc_ping, 1);
  CHECK(sender.clockSync().isValid());
  CHECK_EQ(sender.clockSync().sampleCount(), 1);
}

void testInviteAndUninvite() {
  Link link;
  AOOSender sender(1, link.sender_io);
  AOOTestReceiver receiver(2, link.receiver_io);
  receiver.begin(receiverConfig(receiver));
  auto cfg = sender.defaultConfig();
  cfg.copyFrom(info);
  sender.begin(cfg);  // no sink targets
  receiver.process();
  CHECK_EQ(receiver.getSourceCount(), 0);

  CHECK(receiver.invite(1));
  sender.receive();
  CHECK_EQ(sender.sinkTargets().size(), 1);
  writeBlock(sender, 0);  // PCM: /start is sent with the first block
  receiver.process();
  CHECK_EQ(receiver.getSourceCount(), 1);
  CHECK_EQ(receiver.stats().segments_stored, 1);

  CHECK(receiver.uninvite(1));
  sender.receive();
  CHECK_EQ(sender.sinkTargets().size(), 0);
  receiver.process();
  CHECK_EQ(receiver.getSourceCount(), 0);
}

void testStopRequestRemovesSink() {
  Link link;
  AOOSender sender(1, link.sender_io);
  sender.begin(senderConfig(sender, 4));
  AOOStopSource stop;
  stop.source_id = 1;
  stop.sink_id = 4;
  stop.send(link.receiver_io);
  sender.receive();
  CHECK_EQ(sender.sinkTargets().size(), 0);
}

void testTwoSourcesAreMixed() {
  AOOPacketQueue to_receiver, to_s1, to_s2;
  AOOStreamPipe s1_io{to_s1, 2}, s2_io{to_s2, 2}, r_io{to_receiver, 1};
  s1_io.addOutput(to_receiver);
  s2_io.addOutput(to_receiver);
  r_io.addOutput(to_s1);
  r_io.addOutput(to_s2);
  AOOSender s1(1, s1_io), s2(2, s2_io);
  AOOReceiver receiver(1, r_io);
  receiver.begin(receiverConfig(receiver));
  s1.begin(senderConfig(s1));
  s2.begin(senderConfig(s2));

  int16_t block[BLOCK_SAMPLES];
  for (int b = 0; b < 6; b++) {
    for (auto &v : block) v = 1000;
    s1.write((const uint8_t *)block, BLOCK_BYTES);
    for (auto &v : block) v = 3000;
    s2.write((const uint8_t *)block, BLOCK_BYTES);
  }
  auto out = pullSamples(receiver, BLOCK_SAMPLES);
  CHECK_EQ(receiver.getSourceCount(), 2);
  CHECK_EQ(out.size(), BLOCK_SAMPLES);
  // the mixed signal must contain both sources
  CHECK(out.size() > 0 && out[0] != 1000 && out[0] != 3000 && out[0] != 0);
}

void testStaleSourceIsRemoved() {
  Link link;
  AOOSender sender(1, link.sender_io);
  AOOTestReceiver receiver(1, link.receiver_io);
  auto cfg = receiverConfig(receiver);
  cfg.stream_timeout_ms = 50;
  receiver.begin(cfg);
  sender.begin(senderConfig(sender));
  for (int b = 0; b < 3; b++) writeBlock(sender, b);
  receiver.process();
  receiver.postProcess();
  CHECK_EQ(receiver.getSourceCount(), 1);
  delay(100);
  receiver.postProcess();
  CHECK_EQ(receiver.getSourceCount(), 0);
}

void testWrongSinkIdIsIgnored() {
  Link link;
  AOOSender sender(1, link.sender_io);
  AOOTestReceiver receiver(5, link.receiver_io);
  receiver.begin(receiverConfig(receiver));
  sender.begin(senderConfig(sender, 6));
  writeBlock(sender, 0);
  receiver.process();
  CHECK_EQ(receiver.getSourceCount(), 0);
  CHECK_EQ(receiver.stats().segments_stored, 0);
  CHECK(receiver.messageStats().osc_start == 0);
}

void testBinaryDataIsAccepted() {
  Link link;
  AOOSender sender(1, link.sender_io);
  AOOTestReceiver receiver(1, link.receiver_io);
  receiver.begin(receiverConfig(receiver));
  sender.begin(senderConfig(sender));
  writeBlock(sender, 0);  // /start and seq 0
  AOOStart start;
  CHECK(aooFindStart(link.to_receiver, start));
  receiver.process();

  // binary data message (official aoo format) for sink 1 from source 1
  uint8_t msg[4 + 12 + 4] = {0x80, 0x00, 1, 1};
  int pos = 4;
  auto be32 = [&](int32_t v) {
    for (int i = 3; i >= 0; i--) msg[pos++] = (uint8_t)(v >> (i * 8));
  };
  be32(start.stream_id);
  be32(1);         // seq
  msg[pos++] = 0;  // channel onset
  msg[pos++] = 0;  // flags
  msg[pos++] = 0;  // data size (big endian)
  msg[pos++] = 4;
  link.to_receiver.emplace_back(msg, msg + sizeof(msg));
  receiver.process();
  CHECK_EQ(receiver.messageStats().binary_data, 1);
  CHECK_EQ(receiver.stats().segments_stored, 2);
}

const AOOTestCase tests[] = {
    {"RingBufferIndexing", testRingBufferIndexing},
    {"BufferViewPrimingAndOrder", testBufferViewPrimingAndOrder},
    {"BufferViewGapIsSilence", testBufferViewGapIsSilence},
    {"PacketRecoveryApi", testPacketRecoveryApi},
    {"NtpConversion", testNtpConversion},
    {"ClockSync", testClockSync},
    {"SenderEmitsStartAndData", testSenderEmitsStartAndData},
    {"SenderAnswersResendRequest", testSenderAnswersResendRequest},
    {"PcmLoopbackDeliversBufferedAudio", testPcmLoopbackDeliversBufferedAudio},
    {"MultiFrameReassembly", testMultiFrameReassembly},
    {"RedundantSendsEachBlockTwice", testRedundantSendsEachBlockTwice},
    {"PingPongUpdatesClockSync", testPingPongUpdatesClockSync},
    {"InviteAndUninvite", testInviteAndUninvite},
    {"StopRequestRemovesSink", testStopRequestRemovesSink},
    {"TwoSourcesAreMixed", testTwoSourcesAreMixed},
    {"StaleSourceIsRemoved", testStaleSourceIsRemoved},
    {"WrongSinkIdIsIgnored", testWrongSinkIdIsIgnored},
    {"BinaryDataIsAccepted", testBinaryDataIsAccepted},
};

void setup() {
  Serial.begin(115200);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Error);
  int failed = runTests(tests, sizeof(tests) / sizeof(tests[0]));
  finishTests(failed);
}

void loop() {}
