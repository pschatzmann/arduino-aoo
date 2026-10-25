/**
 * @brief Regression tests for defects found in the code reviews. Each test
 * describes the expected behavior, so that a defect can not come back
 * unnoticed. Run with `ctest -L regression` from the build directory.
 */
#include "../test-support/AOOTestSupport.h"

// ------------------------------------------------------------------
// Receive pipeline
// ------------------------------------------------------------------

/// available() pulls through the decoder pipeline, whose blocking Stream
/// reads wait for the 1s default timeout while the buffer is not primed.
void testAvailableDoesNotBlockBeforePriming() {
  Link link;
  AOOSender sender(1, link.sender_io);
  AOOReceiver receiver(1, link.receiver_io);
  receiver.begin(receiverConfig(receiver));
  sender.begin(senderConfig(sender));
  writeBlock(sender, 0);
  uint32_t start = millis();
  receiver.available();
  uint32_t duration = millis() - start;
  CHECK(duration < 100);
}

/// When the reader catches up with the writer, the next (not yet received)
/// segment must not be skipped.
void testUnderrunDoesNotDropNextSegment() {
  IndexedRingBuffer<SingleBuffer<uint8_t>> buffer(8);
  IndexedRingBufferStreamView view;
  view.setBuffer(&buffer);
  view.setStartSeq(-1);
  uint8_t seg[4];
  for (int s = 0; s < 4; s++) {
    memset(seg, s + 1, sizeof(seg));
    buffer.write(s, seg, sizeof(seg));
    view.notifySegmentReceived(s);
  }
  uint8_t out[16];
  CHECK_EQ(view.readBytes(out, 16), 16);
  view.readBytes(out, 4);  // underrun: nothing new available

  memset(seg, 5, sizeof(seg));
  buffer.write(4, seg, sizeof(seg));
  view.notifySegmentReceived(4);
  memset(out, 0, sizeof(out));
  view.readBytes(out, 4);
  CHECK_EQ(out[0], 5);
}

/// Steady state streaming over a lossless link must be bit exact
void testPcmStreamingIsBitExact() {
  Link link;
  AOOSender sender(1, link.sender_io);
  AOOReceiver receiver(1, link.receiver_io);
  receiver.begin(receiverConfig(receiver));
  sender.begin(senderConfig(sender));
  int b = 0;
  for (; b < 4; b++) writeBlock(sender, b);

  std::vector<int16_t> out;
  for (; b < 40; b++) {
    writeBlock(sender, b);
    auto block = pullSamples(receiver, BLOCK_SAMPLES);
    out.insert(out.end(), block.begin(), block.end());
  }
  CHECK_EQ(out.size(), 36 * BLOCK_SAMPLES);
  CHECK_EQ(rampMismatches(out), 0);
}

/// The sender splits blocks into frames which fit into max_packet_size, the
/// receiver must use the same offsets when reassembling
void testMultiFrameContentIsCorrect() {
  Link link;
  AOOSender sender(1, link.sender_io);
  AOOReceiver receiver(1, link.receiver_io);
  receiver.begin(receiverConfig(receiver));
  auto cfg = senderConfig(sender);
  cfg.max_packet_size = 200;  // 256 byte blocks -> 86 + 86 + 84
  sender.begin(cfg);
  for (int b = 0; b < 6; b++) writeBlock(sender, b);
  auto out = pullSamples(receiver, 4 * BLOCK_SAMPLES);
  CHECK_EQ(out.size(), 4 * BLOCK_SAMPLES);
  CHECK_EQ(rampMismatches(out), 0);
}

/// With the default configuration no packet may exceed max_packet_size: a
/// 1024 byte PCM block (256 stereo frames) used to be sent as one 1060 byte
/// packet, which was fragmented on a network with an MTU of 1024 and then
/// dropped by the ESP32-P4
void testDefaultPacketsFitMaxPacketSize() {
  Link link;
  AOOSender sender(1, link.sender_io);
  auto cfg = senderConfig(sender);
  sender.begin(cfg);
  int16_t block[512] = {0};
  for (int b = 0; b < 4; b++) sender.write((const uint8_t *)block, sizeof(block));
  int frames = 0;
  for (auto &p : link.to_receiver) {
    AOOData data;
    if (!data.parse(p.data(), p.size())) continue;
    frames++;
    CHECK(p.size() <= (size_t)cfg.max_packet_size);
  }
  CHECK_EQ(frames, 4 * 2);  // 1024 bytes -> 2 frames of 512 bytes
}

/// A freshly allocated slot must not look like an active segment with id 0
void testEmptySlotsAreInactive() {
  IndexedRingBuffer<SingleBuffer<uint8_t>> buffer(4);
  CHECK_EQ(buffer.activeCount(), 0);
  CHECK(buffer.get(0) == nullptr);
}

// ------------------------------------------------------------------
// Packet recovery and statistics
// ------------------------------------------------------------------

/// A lost packet must trigger a resend request (AOOPacketRecovery is never
/// notified about received sequence numbers)
void testLostPacketIsRecovered() {
  Link link;
  AOOSender sender(1, link.sender_io);
  AOOTestReceiver receiver(1, link.receiver_io);
  auto cfg = receiverConfig(receiver);
  cfg.recovery_wait_ms = 10;
  receiver.begin(cfg);
  sender.begin(senderConfig(sender));
  bool dropped = false;
  link.sender_io.drop_filter = [&](const uint8_t *data, size_t len) {
    if (!dropped && aooDataSeq(data, len) == 5) return dropped = true;
    return false;
  };
  for (int b = 0; b < 10; b++) writeBlock(sender, b);
  receiver.process();
  receiver.postProcess();
  delay(20);
  receiver.postProcess();
  CHECK(dropped);
  CHECK(receiver.stats().resend_requests > 0);
  sender.receive();
  receiver.process();
  CHECK_EQ(receiver.stats().segments_stored, 10);
}

/// Duplicates sent for redundancy must not be counted as received segments
void testRedundancyKeepsStatsConsistent() {
  Link link;
  AOOSender sender(1, link.sender_io);
  AOOTestReceiver receiver(1, link.receiver_io);
  receiver.begin(receiverConfig(receiver));
  auto cfg = senderConfig(sender);
  cfg.redundancy = 2;
  sender.begin(cfg);
  for (int b = 0; b < 10; b++) writeBlock(sender, b);
  receiver.process();
  receiver.postProcess();
  CHECK_EQ(receiver.xrunCount(), 0);
}

/// Resent data must go to the sink that requested it, not to the last
/// sink the stream was targeted at
void testResendGoesToRequestingSink() {
  Link link;
  AOOSender sender(1, link.sender_io);
  IPAddress sink1(127, 0, 0, 10), sink2(127, 0, 0, 11);
  auto cfg = senderConfig(sender);
  cfg.sink_targets.clear();
  cfg.sink_targets.push_back(AOOSinkTarget(1, (uint32_t)sink1, 9000));
  cfg.sink_targets.push_back(AOOSinkTarget(2, (uint32_t)sink2, 9000));
  sender.begin(cfg);
  for (int b = 0; b < 4; b++) writeBlock(sender, b);
  link.to_receiver.clear();
  link.sender_io.sent_to.clear();

  AOOResendData req;
  req.source_id = 1;
  req.sink_id = 1;
  req.resend_items.push_back({2, 0});
  req.send(link.receiver_io);
  link.sender_io.peer_ip = sink1;
  sender.receive();

  bool found = false;
  for (size_t i = 0; i < link.to_receiver.size(); i++) {
    auto &p = link.to_receiver[i];
    if (aooDataSeq(p.data(), p.size()) == 2) {
      found = true;
      CHECK(link.sender_io.sent_to[i] == (uint32_t)sink1);
    }
  }
  CHECK(found);
}

// ------------------------------------------------------------------
// Receiver setup and stream management
// ------------------------------------------------------------------

/// The default constructor does not register the receiver as listener
void testDefaultConstructedReceiverWorks() {
  Link link;
  AOOSender sender(1, link.sender_io);
  AOOTestReceiver receiver;
  receiver.setStream(link.receiver_io);
  auto cfg = receiverConfig(receiver);
  cfg.id = 1;
  receiver.begin(cfg);
  sender.begin(senderConfig(sender));
  writeBlock(sender, 0);
  receiver.process();
  CHECK_EQ(receiver.getSourceCount(), 1);
}

/// readBytes() returns before processing messages when there is no source
/// yet, so a reader that does not call available() never gets any data
void testReadBytesAloneProcessesMessages() {
  Link link;
  AOOSender sender(1, link.sender_io);
  AOOReceiver receiver(1, link.receiver_io);
  receiver.begin(receiverConfig(receiver));
  sender.begin(senderConfig(sender));
  for (int b = 0; b < 6; b++) writeBlock(sender, b);
  uint8_t buf[BLOCK_BYTES];
  receiver.readBytes(buf, sizeof(buf));
  CHECK_EQ(receiver.getSourceCount(), 1);
}

/// A receiver that missed the /start message (e.g. started after the
/// sender) should ask the source for it
void testLateReceiverRequestsStart() {
  Link link;
  AOOSender sender(1, link.sender_io);
  AOOTestReceiver receiver(1, link.receiver_io);
  receiver.begin(receiverConfig(receiver));
  link.sender_io.drop_filter = [](const uint8_t *data, size_t len) {
    return aooAddressContains(data, len, "/start");
  };
  sender.begin(senderConfig(sender));
  for (int b = 0; b < 6; b++) writeBlock(sender, b);
  receiver.process();
  CHECK(aooCountAddress(link.to_sender, "/aoo/src/1/start") > 0);
}

/// After a pause longer than stream_timeout_ms the stream must resume
void testSourceRecoversAfterTimeout() {
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
  delay(100);
  receiver.postProcess();
  CHECK_EQ(receiver.getSourceCount(), 0);

  uint32_t stored = receiver.stats().segments_stored;
  for (int b = 3; b < 6; b++) {
    writeBlock(sender, b);
    receiver.process();
    sender.receive();
  }
  receiver.process();
  CHECK(receiver.stats().segments_stored > stored);
}

/// A source which sends /start but no audio data (e.g. because the network
/// drops fragmented packets) must be reported once and removed after
/// stream_timeout_ms
void testSourceWithoutDataIsReportedAndRemoved() {
  Link link;
  AOOSender sender(1, link.sender_io);
  AOOTestReceiver receiver(1, link.receiver_io);
  auto cfg = receiverConfig(receiver);
  cfg.no_data_warning_ms = 30;
  cfg.stream_timeout_ms = 100;
  receiver.begin(cfg);
  sender.begin(senderConfig(sender));
  link.sender_io.drop_filter = [](const uint8_t *data, size_t len) {
    return aooAddressContains(data, len, "/data");
  };
  for (int b = 0; b < 3; b++) writeBlock(sender, b);
  receiver.process();
  receiver.postProcess();
  CHECK_EQ(receiver.getSourceCount(), 1);
  CHECK_EQ(receiver.stats().no_data_warnings, 0);
  delay(50);
  receiver.postProcess();
  receiver.postProcess();
  CHECK_EQ(receiver.stats().no_data_warnings, 1);
  delay(80);
  receiver.postProcess();
  CHECK_EQ(receiver.getSourceCount(), 0);
}

/// With id 0 the receiver auto-assigns its id from the first message: a
/// message with a different address prefix must not set it to -1
void testAutoIdIgnoresForeignMessages() {
  Link link;
  AOOSender sender(1, link.sender_io);
  AOOTestReceiver receiver(0, link.receiver_io);
  receiver.begin(receiverConfig(receiver));
  AOOPacketQueue unused;
  AOOStreamPipe other{unused};
  other.addOutput(link.to_receiver);
  AOOPingSource foreign;
  foreign.source_id = 5;
  foreign.sink_id = 9;
  foreign.send(other);
  sender.begin(senderConfig(sender));
  writeBlock(sender, 0);
  receiver.process();
  CHECK_EQ(receiver.getSourceCount(), 1);
}

// ------------------------------------------------------------------
// Second review round
// ------------------------------------------------------------------

/// The stream id must be random: random(INT32_MIN, INT32_MAX) overflows
/// where long has 32 bits and always returned INT32_MIN
void testStreamIdsAreRandom() {
  int32_t ids[2];
  for (int j = 0; j < 2; j++) {
    Link link;
    AOOSender sender(1, link.sender_io);
    sender.begin(senderConfig(sender));
    writeBlock(sender, 0);  // /start is sent with the first block
    delay(2);
    AOOStart start;
    CHECK(aooFindStart(link.to_receiver, start));
    ids[j] = start.stream_id;
    CHECK(ids[j] != INT32_MIN);
    CHECK(ids[j] != 0);
  }
  CHECK(ids[0] != ids[1]);
}

/// The adaptive resampling must follow the buffer fill level (and not
/// estimate the rate from the arrival of packets): a full buffer plays
/// faster, an empty buffer slower, always within max_resample_percent
void testAdaptiveResamplingFollowsBufferFill() {
  Link link;
  AOOSender sender(1, link.sender_io);
  AOOTestReceiver receiver(1, link.receiver_io);
  auto cfg = receiverConfig(receiver);
  cfg.adaptive_resampling = true;
  cfg.resample_update_ms = 0;
  cfg.max_resample_percent = 0.5f;
  receiver.begin(cfg);
  sender.begin(senderConfig(sender));
  const int max_rate = 44100 * 1.005 + 1, min_rate = 44100 * 0.995 - 1;

  // buffer full: the sender is faster than the playback
  for (int b = 0; b < 8; b++) writeBlock(sender, b);
  for (int j = 0; j < 5; j++) {
    receiver.process();
    receiver.postProcess();
  }
  CHECK(receiver.resampleRate() > 44100);
  CHECK(receiver.resampleRate() <= max_rate);

  // the audio continues while the rate changes: the resampled output of the
  // segments must not get lost (the buffered resampler flushed its output,
  // which clears the QueueStream)
  auto out = pullSamples(receiver, 2 * BLOCK_SAMPLES);
  CHECK(out.size() >= 2 * BLOCK_SAMPLES * 95 / 100);

  // buffer (almost) empty: the sender is slower than the playback
  for (int j = 0; j < 10; j++) {
    pullSamples(receiver, 2 * BLOCK_SAMPLES);
    receiver.postProcess();
  }
  CHECK(receiver.resampleRate() < 44100);
  CHECK(receiver.resampleRate() >= min_rate);
}

/// Losses at the start of a stream must not suppress resend requests for a
/// long time (the quality gate used the cumulative quality)
void testQualityGateUsesRecentWindow() {
  Link link;
  AOOSender sender(1, link.sender_io);
  AOOTestReceiver receiver(1, link.receiver_io);
  auto cfg = receiverConfig(receiver);
  cfg.buffer_depth = 200;
  cfg.recovery_wait_ms = 1;
  receiver.begin(cfg);
  sender.begin(senderConfig(sender));
  // lose 4 packets early and one after 160 blocks: cumulative quality 96.9%
  link.sender_io.drop_filter = [](const uint8_t *data, size_t len) {
    int32_t seq = aooDataSeq(data, len);
    return (seq >= 2 && seq <= 5) || seq == 160;
  };
  for (int b = 0; b < 165; b++) {
    writeBlock(sender, b);
    receiver.process();
  }
  delay(5);
  receiver.postProcess();
  CHECK(receiver.stats().resend_requests > 0);
}

/// Redundant copies of the frames of a multi-frame block must not corrupt
/// the reassembly
void testMultiFrameWithRedundancy() {
  Link link;
  AOOSender sender(1, link.sender_io);
  AOOReceiver receiver(1, link.receiver_io);
  receiver.begin(receiverConfig(receiver));
  auto cfg = senderConfig(sender);
  cfg.max_packet_size = 200;
  cfg.redundancy = 2;
  sender.begin(cfg);
  for (int b = 0; b < 6; b++) writeBlock(sender, b);
  auto out = pullSamples(receiver, 4 * BLOCK_SAMPLES);
  CHECK_EQ(receiver.stats().assembly_completed, 6);
  CHECK_EQ(out.size(), 4 * BLOCK_SAMPLES);
  CHECK_EQ(rampMismatches(out), 0);
}

/// A resent block must be split like the original, so that no packet
/// exceeds max_packet_size
void testResendSplitsLargeBlocks() {
  Link link;
  AOOSender sender(1, link.sender_io);
  auto cfg = senderConfig(sender);
  cfg.max_packet_size = 200;
  sender.begin(cfg);
  for (int b = 0; b < 4; b++) writeBlock(sender, b);
  link.to_receiver.clear();
  AOOResendData req;
  req.source_id = 1;
  req.sink_id = 1;
  req.resend_items.push_back({2, 0});
  req.send(link.receiver_io);
  sender.receive();
  int frames = 0;
  for (auto &p : link.to_receiver) {
    AOOData data;
    if (!data.parse(p.data(), p.size()) || data.seq_no != 2) continue;
    frames++;
    CHECK(p.size() <= 200);
    CHECK_EQ(data.total_number_of_frames, 3);
  }
  CHECK_EQ(frames, 3);
}

/// The source must answer a ping with a pong to the sink that pinged
void testPongGoesToPinger() {
  Link link;
  AOOSender sender(1, link.sender_io);
  IPAddress sink1(127, 0, 0, 10), sink2(127, 0, 0, 11);
  auto cfg = senderConfig(sender);
  cfg.sink_targets.clear();
  cfg.sink_targets.push_back(AOOSinkTarget(1, (uint32_t)sink1, 9000));
  cfg.sink_targets.push_back(AOOSinkTarget(2, (uint32_t)sink2, 9000));
  sender.begin(cfg);
  writeBlock(sender, 0);  // sends pings, the last target is sink2
  link.to_receiver.clear();
  link.sender_io.sent_to.clear();

  AOOPingSource ping;
  ping.source_id = 1;
  ping.sink_id = 1;
  ping.send_time = 1000;
  ping.send(link.receiver_io);
  link.sender_io.peer_ip = sink1;
  sender.receive();
  int pongs = 0;
  for (size_t i = 0; i < link.to_receiver.size(); i++) {
    auto &p = link.to_receiver[i];
    if (!aooAddressContains(p.data(), p.size(), "/pong")) continue;
    pongs++;
    CHECK(link.sender_io.sent_to[i] == (uint32_t)sink1);
  }
  CHECK_EQ(pongs, 1);
}

/// A message which is ignored must not delay the following messages
void testStrayMessageDoesNotBlockProcessing() {
  Link link;
  AOOSender sender(1, link.sender_io);
  AOOTestReceiver receiver(1, link.receiver_io);
  receiver.begin(receiverConfig(receiver));
  sender.begin(senderConfig(sender));
  link.to_receiver.push_front({'/', 'x', 0, 0, ',', 0, 0, 0});  // foreign
  writeBlock(sender, 0);
  receiver.process();
  CHECK_EQ(receiver.getSourceCount(), 1);
  CHECK_EQ(receiver.stats().segments_stored, 1);
}

// ------------------------------------------------------------------
// Third review round
// ------------------------------------------------------------------

/// Messages must be read with one readBytes() call per datagram: through an
/// AOOStream* the non virtual Stream::readBytes() read byte by byte
void testMessagesAreReadInBulk() {
  Link link;
  AOOSender sender(1, link.sender_io);
  AOOTestReceiver receiver(1, link.receiver_io);
  receiver.begin(receiverConfig(receiver));
  sender.begin(senderConfig(sender));
  writeBlock(sender, 0);
  receiver.process();
  CHECK(link.receiver_io.bulk_reads > 0);
  CHECK_EQ(receiver.stats().segments_stored, 1);
}

/// The configured output format must be kept: the sources are converted
void testOutputFormatIsKept() {
  Link link;
  AOOSender sender(1, link.sender_io);
  AOOReceiver receiver(1, link.receiver_io);
  auto cfg = receiverConfig(receiver);
  cfg.sample_rate = 48000;
  cfg.channels = 2;
  receiver.begin(cfg);
  sender.begin(senderConfig(sender));  // 44100 Hz mono
  for (int b = 0; b < 6; b++) writeBlock(sender, b);
  auto out = pullSamples(receiver, BLOCK_SAMPLES);
  CHECK(out.size() > 0);
  CHECK_EQ(receiver.audioInfo().sample_rate, 48000);
  CHECK_EQ(receiver.audioInfo().channels, 2);
}

/// With output_follows_source the first source defines the output format
void testOutputFollowsSource() {
  Link link;
  AOOSender sender(1, link.sender_io);
  AOOTestReceiver receiver(1, link.receiver_io);
  auto cfg = receiverConfig(receiver);
  cfg.sample_rate = 48000;
  cfg.channels = 2;
  cfg.output_follows_source = true;
  receiver.begin(cfg);
  sender.begin(senderConfig(sender));
  writeBlock(sender, 0);
  receiver.process();
  CHECK_EQ(receiver.audioInfo().sample_rate, 44100);
  CHECK_EQ(receiver.audioInfo().channels, 1);
}

/// end() must tell the sinks that the stream stopped
void testSenderSendsStopOnEnd() {
  Link link;
  AOOSender sender(1, link.sender_io);
  AOOTestReceiver receiver(1, link.receiver_io);
  receiver.begin(receiverConfig(receiver));
  sender.begin(senderConfig(sender));
  writeBlock(sender, 0);
  receiver.process();
  CHECK_EQ(receiver.getSourceCount(), 1);
  sender.end();
  sender.end();  // only one stop message
  CHECK_EQ(aooCountAddress(link.to_receiver, "/stop"), 1);
  receiver.process();
  CHECK_EQ(receiver.getSourceCount(), 0);
}

/// /start must announce the block size which is actually sent: for PCM it is
/// sent with the first block (and not with an estimate in begin())
void testStartAnnouncesActualBlockSize() {
  Link link;
  AOOSender sender(1, link.sender_io);
  sender.begin(senderConfig(sender));
  writeBlock(sender, 0);
  writeBlock(sender, 1);
  int32_t block_size = -1;
  for (auto &p : link.to_receiver) {
    AOOStart start;
    if (aooAddressContains(p.data(), p.size(), "/start") &&
        start.parse(p.data(), p.size()))
      block_size = start.block_size;
  }
  CHECK_EQ(block_size, BLOCK_SAMPLES);
  CHECK_EQ(aooCountAddress(link.to_receiver, "/start"), 1);
}

/// AOOReceiverTask processes the messages in its own task
void testReceiverTaskStreams() {
  Link link;
  AOOSender sender(1, link.sender_io);
  AOOReceiverTask receiver(1, link.receiver_io);
  CHECK(receiver.begin(receiverConfig(receiver)));
  sender.begin(senderConfig(sender));
  for (int b = 0; b < 6; b++) writeBlock(sender, b);
  for (int j = 0; j < 100 && receiver.stats().segments_stored < 6; j++) {
    delay(10);
  }
  CHECK_EQ(receiver.stats().segments_stored, 6);
  auto out = pullSamples(receiver, 4 * BLOCK_SAMPLES);
  CHECK_EQ(out.size(), 4 * BLOCK_SAMPLES);
  CHECK_EQ(rampMismatches(out), 0);
  receiver.end();
}

// ------------------------------------------------------------------
// Final review
// ------------------------------------------------------------------

/// A /start which can not be parsed must not create a source line
void testMalformedStartCreatesNoSource() {
  Link link;
  AOOTestReceiver receiver(1, link.receiver_io);
  receiver.begin(receiverConfig(receiver));
  // address of a /start, but a wrong format
  AOOStopSink wrong;
  wrong.source_id = 1;
  wrong.sink_id = 1;
  AOOPacketQueue unused;
  AOOStreamPipe other{unused};
  other.addOutput(link.to_receiver);
  wrong.send(other);
  auto &p = link.to_receiver.back();
  // both addresses are padded to 20 bytes: the format stays at its position
  const char *stop = "/aoo/sink/1/stop";
  const char *start = "/aoo/sink/1/start";
  CHECK(memcmp(p.data(), stop, strlen(stop)) == 0);
  memcpy(p.data(), start, strlen(start) + 1);
  receiver.process();
  CHECK_EQ(receiver.getSourceCount(), 0);
}

/// A repeated /start for an unchanged stream must keep the buffered audio
void testRepeatedStartKeepsBuffer() {
  Link link;
  AOOSender sender(1, link.sender_io);
  AOOReceiver receiver(1, link.receiver_io);
  receiver.begin(receiverConfig(receiver));
  sender.begin(senderConfig(sender));
  for (int b = 0; b < 6; b++) writeBlock(sender, b);
  AOOStart start;
  CHECK(aooFindStart(link.to_receiver, start));
  receiver.available();  // processes /start and the data
  // the same /start again (e.g. the answer to a /start request)
  start.send(link.sender_io);
  auto out = pullSamples(receiver, 4 * BLOCK_SAMPLES);
  CHECK_EQ(out.size(), 4 * BLOCK_SAMPLES);
  CHECK_EQ(rampMismatches(out), 0);
}

/// AOOReceiverTask can be ended while streaming and started again: the task
/// must not be stopped while it holds the lock
void testReceiverTaskRestarts() {
  for (int run = 0; run < 2; run++) {
    Link link;
    AOOSender sender(1, link.sender_io);
    AOOReceiverTask receiver(1, link.receiver_io);
    CHECK(receiver.begin(receiverConfig(receiver)));
    sender.begin(senderConfig(sender));
    for (int b = 0; b < 6; b++) writeBlock(sender, b);
    for (int j = 0; j < 100 && receiver.stats().segments_stored < 6; j++)
      delay(10);
    CHECK_EQ(receiver.stats().segments_stored, 6);
    receiver.end();
    CHECK(receiver.begin(receiverConfig(receiver)));
    for (int b = 6; b < 12; b++) writeBlock(sender, b);
    for (int j = 0; j < 100 && receiver.getSourceCount() == 0; j++)
      delay(10);
    CHECK_EQ(receiver.getSourceCount(), 1);
    receiver.end();
  }
}

const AOOTestCase tests[] = {
    {"AvailableDoesNotBlockBeforePriming",
     testAvailableDoesNotBlockBeforePriming},
    {"UnderrunDoesNotDropNextSegment", testUnderrunDoesNotDropNextSegment},
    {"PcmStreamingIsBitExact", testPcmStreamingIsBitExact},
    {"MultiFrameContentIsCorrect", testMultiFrameContentIsCorrect},
    {"DefaultPacketsFitMaxPacketSize", testDefaultPacketsFitMaxPacketSize},
    {"EmptySlotsAreInactive", testEmptySlotsAreInactive},
    {"LostPacketIsRecovered", testLostPacketIsRecovered},
    {"RedundancyKeepsStatsConsistent", testRedundancyKeepsStatsConsistent},
    {"ResendGoesToRequestingSink", testResendGoesToRequestingSink},
    {"DefaultConstructedReceiverWorks", testDefaultConstructedReceiverWorks},
    {"ReadBytesAloneProcessesMessages", testReadBytesAloneProcessesMessages},
    {"LateReceiverRequestsStart", testLateReceiverRequestsStart},
    {"SourceRecoversAfterTimeout", testSourceRecoversAfterTimeout},
    {"SourceWithoutDataIsReportedAndRemoved",
     testSourceWithoutDataIsReportedAndRemoved},
    {"AutoIdIgnoresForeignMessages", testAutoIdIgnoresForeignMessages},
    {"StreamIdsAreRandom", testStreamIdsAreRandom},
    {"AdaptiveResamplingFollowsBufferFill",
     testAdaptiveResamplingFollowsBufferFill},
    {"QualityGateUsesRecentWindow", testQualityGateUsesRecentWindow},
    {"MultiFrameWithRedundancy", testMultiFrameWithRedundancy},
    {"ResendSplitsLargeBlocks", testResendSplitsLargeBlocks},
    {"PongGoesToPinger", testPongGoesToPinger},
    {"StrayMessageDoesNotBlockProcessing",
     testStrayMessageDoesNotBlockProcessing},
    {"MessagesAreReadInBulk", testMessagesAreReadInBulk},
    {"OutputFormatIsKept", testOutputFormatIsKept},
    {"OutputFollowsSource", testOutputFollowsSource},
    {"SenderSendsStopOnEnd", testSenderSendsStopOnEnd},
    {"StartAnnouncesActualBlockSize", testStartAnnouncesActualBlockSize},
    {"ReceiverTaskStreams", testReceiverTaskStreams},
    {"MalformedStartCreatesNoSource", testMalformedStartCreatesNoSource},
    {"RepeatedStartKeepsBuffer", testRepeatedStartKeepsBuffer},
    {"ReceiverTaskRestarts", testReceiverTaskRestarts},
};

void setup() {
  Serial.begin(115200);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Error);
  int failed = runTests(tests, sizeof(tests) / sizeof(tests[0]));
  finishTests(failed);
}

void loop() {}
