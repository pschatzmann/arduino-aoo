/**
 *   @brief  Test cases for all AOO messages. For each message type we
 * - fill all attributes
 * - Serialize the message
 * - parse the serialized message
 * - check the parsed data
 */

#include "AudioTools.h"
#include "AOO.h"

SingleBuffer<uint8_t> buffer{1024 * 3};
QueueStream<uint8_t> queue{buffer};

void testAOOStart() {
  Serial.println("testAOOStart");
  buffer.clear();
  // create a stream to send the data
  AOOStart aoo_start;
  aoo_start.source_id = 1;
  aoo_start.sink_id = 2;
  aoo_start.version = "1.0.0";
  aoo_start.stream_id = 3;
  aoo_start.sequence_number_start = 4;
  aoo_start.format_id = 5;
  aoo_start.channels = 6;
  aoo_start.sample_rate = 44100;
  aoo_start.block_size = 1024;
  aoo_start.codec = "pcm";
  aoo_start.codec_extension = {nullptr, 0};
  aoo_start.start_time = 123456789ULL;
  aoo_start.reblock_resample_latency_samples = 7;
  aoo_start.codec_delay_samples = 8;
  aoo_start.sample_offset = 9;
  assert(aoo_start.send(queue));

  // create a stream to receive the data
  AOOStart rcv;
  assert(rcv.parse(buffer.data(), buffer.size()));
  assert(rcv.source_id == 1);
  assert(rcv.sink_id == 2);
  assert(strcmp(rcv.version, "1.0.0") == 0);
  assert(rcv.stream_id == 3);
  assert(rcv.sequence_number_start == 4);
  assert(rcv.format_id == 5);
  assert(rcv.channels == 6);
  assert(rcv.sample_rate == 44100);
  assert(rcv.block_size == 1024);
  assert(strcmp(rcv.codec, "pcm") == 0);
  assert(rcv.codec_extension.len == 0);
  assert(rcv.start_time == 123456789ULL);
  assert(rcv.reblock_resample_latency_samples == 7);
  assert(rcv.codec_delay_samples == 8);
  assert(rcv.sample_offset == 9);
}

void testAOORequestStart() {
  Serial.println("testAOORequestStart");
  buffer.clear();

  AOORequestStart req_start;
  req_start.source_id = 10;
  req_start.sink_id = 20;
  req_start.version = "1.0.0";
  assert(req_start.send(queue));

  AOORequestStart rcv;
  rcv.parse(buffer.data(), buffer.size());
  
  assert(rcv.source_id == 10);
  assert(rcv.sink_id == 20);
  assert(strcmp(rcv.version, "1.0.0") == 0);
}

void testAOOStopSink() {
  Serial.println("testAOOStopSink");
  buffer.clear();

  AOOStopSink sink_stop;
  sink_stop.source_id = 5;
  sink_stop.sink_id = 15;
  sink_stop.stream_id = 25;
  sink_stop.last_seq = 99;
  sink_stop.sample_offset = 1000;
  assert(sink_stop.send(queue));

  AOOStopSink rcv;
  rcv.parse(buffer.data(), buffer.size());

  assert(rcv.source_id == 5);
  assert(rcv.sink_id == 15);
  assert(rcv.stream_id == 25);
  assert(rcv.last_seq == 99);
  assert(rcv.sample_offset == 1000);
}

void testAOOStopSource() {
  Serial.println("testAOOStopSource");
  buffer.clear();

  AOOStopSource source_stop;
  source_stop.source_id = 6;
  source_stop.sink_id = 16;
  source_stop.stream_id = 26;
  assert(source_stop.send(queue));

  AOOStopSource rcv;
  rcv.parse(buffer.data(), buffer.size());
  
  assert(rcv.source_id == 6);
  assert(rcv.sink_id == 16);
  assert(rcv.stream_id == 26);
}

void testAOOData() {
  Serial.println("testAOOData");
  buffer.clear();

  uint8_t audio_buffer[100];
  // Fill with sample data
  for (int i = 0; i < 100; i++) {
    audio_buffer[i] = i % 256;
  }

  AOOData data_msg;
  data_msg.source_id = 7;
  data_msg.sink_id = 17;
  data_msg.stream_id = 27;
  data_msg.seq_no = 100;
  data_msg.real_sample_rate = 48000.0;
  data_msg.channel_onset = 0;
  data_msg.total_size = 100;
  data_msg.total_number_of_frames = 1;
  data_msg.frame_idx = 123;
  data_msg.audio_data = {audio_buffer, 100};
  assert(data_msg.send(queue));

  AOOData rcv;
  rcv.parse(buffer.data(), buffer.size());
  
  assert(rcv.source_id == 7);
  assert(rcv.sink_id == 17);
  assert(rcv.stream_id == 27);
  assert(rcv.seq_no == 100);
  assert(rcv.real_sample_rate == 48000.0);
  assert(rcv.channel_onset == 0);
  assert(rcv.total_size == 100);
  assert(rcv.total_number_of_frames == 1);
  assert(rcv.frame_idx == 123);

  for (int i = 0; i < 100; i++) {
    assert(rcv.audio_data.data[i] == i % 256);
  }
}

void testAOOResendData() {
  Serial.println("testAOOResendData");
  buffer.clear();

  AOOResendData resend;
  resend.source_id = 8;
  resend.sink_id = 18;
  resend.stream_id = 28;

  // Add some items to resend
  resend.resend_items.push_back({101, 0});
  resend.resend_items.push_back({102, 1});
  assert(resend.send(queue));

  AOOResendData rcv;
  rcv.parse(buffer.data(), buffer.size());
  
  assert(rcv.source_id == 8);
  assert(rcv.sink_id == 18);
  assert(rcv.stream_id == 28);
  assert(rcv.resend_items.size() == 2);
  assert(rcv.resend_items[0].seq == 101);
  assert(rcv.resend_items[0].frame == 0);
  assert(rcv.resend_items[1].seq == 102);
  assert(rcv.resend_items[1].frame == 1);
}

void testAOOPingSink() {
  Serial.println("testAOOPingSink");
  buffer.clear();

  AOOPingSink ping;
  ping.source_id = 9;
  ping.sink_id = 19;
  ping.send_time = 123456789ULL;
  assert(ping.send(queue));

  AOOPingSink rcv;
  rcv.parse(buffer.data(), buffer.size());
  
  assert(rcv.source_id == 9);
  assert(rcv.sink_id == 19);
  assert(rcv.send_time == 123456789ULL);
}

void testAOOPingSource() {
  Serial.println("testAOOPingSource");
  buffer.clear();

  AOOPingSource ping;
  ping.source_id = 11;
  ping.sink_id = 21;
  ping.send_time = 987654321ULL;
  assert(ping.send(queue));

  AOOPingSource rcv;
  rcv.parse(buffer.data(), buffer.size());
  
  assert(rcv.source_id == 11);
  assert(rcv.sink_id == 21);
  assert(rcv.send_time == 987654321ULL);
}

void testAOOPongSink() {
  Serial.println("testAOOPongSink");
  buffer.clear();

  AOOPongSink pong;
  pong.source_id = 12;
  pong.sink_id = 22;
  pong.t1 = 111111ULL;
  pong.t2 = 222222ULL;
  pong.t3 = 333333ULL;
  assert(pong.send(queue));

  AOOPongSink rcv;
  rcv.parse(buffer.data(), buffer.size());
  
  assert(rcv.source_id == 12);
  assert(rcv.sink_id == 22);
  assert(rcv.t1 == 111111ULL);
  assert(rcv.t2 == 222222ULL);
  assert(rcv.t3 == 333333ULL);
}

void testAOOPongSource() {
  Serial.println("testAOOPongSource");
  buffer.clear();

  AOOPongSource pong;
  pong.source_id = 13;
  pong.sink_id = 23;
  pong.t1 = 444444ULL;
  pong.t2 = 555555ULL;
  pong.t3 = 666666ULL;
  assert(pong.send(queue));

  AOOPongSource rcv;
  rcv.parse(buffer.data(), buffer.size());
  
  assert(rcv.source_id == 13);
  assert(rcv.sink_id == 23);
  assert(rcv.t1 == 444444ULL);
  assert(rcv.t2 == 555555ULL);
  assert(rcv.t3 == 666666ULL);
}

void testAOOInvite() {
  Serial.println("testAOOInvite");
  buffer.clear();

  AOOInvite invite;
  invite.source_id = 30;
  invite.sink_id = 40;
  invite.stream_id = 50;
  assert(invite.send(queue));

  AOOInvite rcv;
  assert(rcv.parse(buffer.data(), buffer.size()));

  assert(rcv.source_id == 30);
  assert(rcv.sink_id == 40);
  assert(rcv.stream_id == 50);
}

void testAOOUninvite() {
  Serial.println("testAOOUninvite");
  buffer.clear();

  AOOUninvite uninvite;
  uninvite.source_id = 31;
  uninvite.sink_id = 41;
  uninvite.stream_id = 51;
  assert(uninvite.send(queue));

  AOOUninvite rcv;
  assert(rcv.parse(buffer.data(), buffer.size()));

  assert(rcv.source_id == 31);
  assert(rcv.sink_id == 41);
  assert(rcv.stream_id == 51);
}

void testAOODecline() {
  Serial.println("testAOODecline");
  buffer.clear();

  AOODecline decline;
  decline.source_id = 32;
  decline.sink_id = 42;
  decline.stream_id = 52;
  assert(decline.send(queue));

  AOODecline rcv;
  assert(rcv.parse(buffer.data(), buffer.size()));

  assert(rcv.source_id == 32);
  assert(rcv.sink_id == 42);
  assert(rcv.stream_id == 52);
}

// Helper: write a big-endian int32 to a buffer
static void write_be_int32(uint8_t* buf, int32_t val) {
  buf[0] = (val >> 24) & 0xFF;
  buf[1] = (val >> 16) & 0xFF;
  buf[2] = (val >> 8) & 0xFF;
  buf[3] = val & 0xFF;
}

// Helper: write a big-endian uint16 to a buffer
static void write_be_uint16(uint8_t* buf, uint16_t val) {
  buf[0] = (val >> 8) & 0xFF;
  buf[1] = val & 0xFF;
}

// Helper: write a big-endian double to a buffer
static void write_be_double(uint8_t* buf, double val) {
  uint64_t bits;
  memcpy(&bits, &val, sizeof(bits));
  for (int i = 7; i >= 0; i--) {
    buf[7 - i] = (bits >> (i * 8)) & 0xFF;
  }
}

// Helper: write a big-endian uint64 to a buffer
static void write_be_uint64(uint8_t* buf, uint64_t val) {
  for (int i = 7; i >= 0; i--) {
    buf[7 - i] = (val >> (i * 8)) & 0xFF;
  }
}

void testBinMsgDetection() {
  Serial.println("testBinMsgDetection");

  // Binary message: high bit set
  uint8_t bin_msg[] = {0x81, 0x00, 0x02, 0x01};
  assert(aoo_is_binary(bin_msg, sizeof(bin_msg)) == true);

  // OSC message: starts with '/'
  uint8_t osc_msg[] = {'/', 'a', 'o', 'o'};
  assert(aoo_is_binary(osc_msg, sizeof(osc_msg)) == false);

  // Too short
  uint8_t short_msg[] = {0x81, 0x00, 0x02};
  assert(aoo_is_binary(short_msg, sizeof(short_msg)) == false);
}

void testBinDataSimple() {
  Serial.println("testBinDataSimple");

  // Build a simple binary data message with small IDs, no optional flags
  // Header: type=0 (source) | 0x80, cmd=0 (data), to=42, from=7
  // Body: stream_id(4) + seq(4) + channel(1) + flags(1) + data_size(2) + audio(10)
  uint8_t msg[4 + 12 + 10];
  memset(msg, 0, sizeof(msg));
  int pos = 0;

  // Header
  msg[pos++] = 0x80;   // type=source | 0x80
  msg[pos++] = 0x00;   // cmd=data, no large IDs
  msg[pos++] = 42;     // to (sink_id)
  msg[pos++] = 7;      // from (source_id)

  // Body
  write_be_int32(msg + pos, 100);  // stream_id
  pos += 4;
  write_be_int32(msg + pos, 55);   // sequence
  pos += 4;
  msg[pos++] = 3;      // channel
  msg[pos++] = 0;      // flags (no optional fields)
  write_be_uint16(msg + pos, 10);  // data_size
  pos += 2;

  // Audio data
  for (int i = 0; i < 10; i++) {
    msg[pos++] = (uint8_t)(i + 1);
  }

  AOOData out;
  assert(aoo_parse_bin_data(msg, pos, out));
  assert(out.sink_id == 42);
  assert(out.source_id == 7);
  assert(out.stream_id == 100);
  assert(out.seq_no == 55);
  assert(out.channel_onset == 3);
  assert(out.total_size == 10);
  assert(out.total_number_of_frames == 1);
  assert(out.frame_idx == 0);
  assert(out.audio_data.len == 10);
  for (int i = 0; i < 10; i++) {
    assert(out.audio_data.data[i] == (uint8_t)(i + 1));
  }
}

void testBinDataWithFlags() {
  Serial.println("testBinDataWithFlags");

  // Build a binary data message with Frames + SampleRate + TimeStamp flags
  uint8_t msg[128];
  memset(msg, 0, sizeof(msg));
  int pos = 0;

  // Header (small IDs)
  msg[pos++] = 0x81;   // type=sink | 0x80
  msg[pos++] = 0x00;   // cmd=data
  msg[pos++] = 10;     // to (sink_id)
  msg[pos++] = 20;     // from (source_id)

  // Body
  write_be_int32(msg + pos, 200);  // stream_id
  pos += 4;
  write_be_int32(msg + pos, 99);   // sequence
  pos += 4;
  msg[pos++] = 0;      // channel
  // flags: Frames(0x02) | SampleRate(0x01) | TimeStamp(0x10)
  msg[pos++] = 0x02 | 0x01 | 0x10;
  write_be_uint16(msg + pos, 8);   // data_size
  pos += 2;

  // Frames: total_size(4) + num_frames(2) + frame_index(2)
  write_be_int32(msg + pos, 1024); // total_size
  pos += 4;
  write_be_uint16(msg + pos, 3);   // num_frames
  pos += 2;
  write_be_uint16(msg + pos, 1);   // frame_index
  pos += 2;

  // SampleRate: double (8 bytes)
  write_be_double(msg + pos, 48000.0);
  pos += 8;

  // TimeStamp: uint64 (8 bytes)
  write_be_uint64(msg + pos, 123456789ULL);
  pos += 8;

  // Audio data (8 bytes)
  for (int i = 0; i < 8; i++) {
    msg[pos++] = (uint8_t)(0xA0 + i);
  }

  AOOData out;
  assert(aoo_parse_bin_data(msg, pos, out));
  assert(out.sink_id == 10);
  assert(out.source_id == 20);
  assert(out.stream_id == 200);
  assert(out.seq_no == 99);
  assert(out.total_size == 1024);
  assert(out.total_number_of_frames == 3);
  assert(out.frame_idx == 1);
  assert(out.real_sample_rate == 48000.0);
  assert(out.timestamp == 123456789ULL);
  assert(out.audio_data.len == 8);
  for (int i = 0; i < 8; i++) {
    assert(out.audio_data.data[i] == (uint8_t)(0xA0 + i));
  }
}

void testBinDataLargeIDs() {
  Serial.println("testBinDataLargeIDs");

  // Build a binary data message with large IDs (12-byte header)
  uint8_t msg[128];
  memset(msg, 0, sizeof(msg));
  int pos = 0;

  // Header (large IDs)
  msg[pos++] = 0x80;         // type=source | 0x80
  msg[pos++] = 0x00 | 0x80;  // cmd=data | large ID flag
  msg[pos++] = 0;            // small to (ignored)
  msg[pos++] = 0;            // small from (ignored)
  write_be_int32(msg + pos, 1000);  // to (sink_id, large)
  pos += 4;
  write_be_int32(msg + pos, 2000);  // from (source_id, large)
  pos += 4;

  // Body
  write_be_int32(msg + pos, 300);  // stream_id
  pos += 4;
  write_be_int32(msg + pos, 77);   // sequence
  pos += 4;
  msg[pos++] = 0;      // channel
  msg[pos++] = 0;      // flags (no optional fields)
  write_be_uint16(msg + pos, 4);   // data_size
  pos += 2;

  // Audio data
  msg[pos++] = 0xDE;
  msg[pos++] = 0xAD;
  msg[pos++] = 0xBE;
  msg[pos++] = 0xEF;

  AOOData out;
  assert(aoo_parse_bin_data(msg, pos, out));
  assert(out.sink_id == 1000);
  assert(out.source_id == 2000);
  assert(out.stream_id == 300);
  assert(out.seq_no == 77);
  assert(out.audio_data.len == 4);
  assert(out.audio_data.data[0] == 0xDE);
  assert(out.audio_data.data[1] == 0xAD);
  assert(out.audio_data.data[2] == 0xBE);
  assert(out.audio_data.data[3] == 0xEF);
}

void testBinDataXRun() {
  Serial.println("testBinDataXRun");

  // XRun message: flags = 0x08, data_size = 0
  uint8_t msg[64];
  memset(msg, 0, sizeof(msg));
  int pos = 0;

  msg[pos++] = 0x80;
  msg[pos++] = 0x00;
  msg[pos++] = 5;   // sink_id
  msg[pos++] = 3;   // source_id

  write_be_int32(msg + pos, 50);  // stream_id
  pos += 4;
  write_be_int32(msg + pos, 10);  // sequence
  pos += 4;
  msg[pos++] = 0;    // channel
  msg[pos++] = 0x08; // flags: XRun
  write_be_uint16(msg + pos, 0);  // data_size = 0
  pos += 2;

  AOOData out;
  assert(aoo_parse_bin_data(msg, pos, out));
  assert(out.sink_id == 5);
  assert(out.source_id == 3);
  assert(out.seq_no == 10);
  assert(out.audio_data.data == nullptr);
  assert(out.audio_data.len == 0);
}

void testBinDataTooShort() {
  Serial.println("testBinDataTooShort");

  // Message too short to contain even a header
  uint8_t msg[] = {0x80, 0x00};
  AOOData out;
  assert(aoo_parse_bin_data(msg, sizeof(msg), out) == false);

  // Header OK but body too short
  uint8_t msg2[] = {0x80, 0x00, 5, 3, 0, 0, 0, 1};
  assert(aoo_parse_bin_data(msg2, sizeof(msg2), out) == false);
}

void setup() {
  Serial.begin(115200);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Info);
  queue.begin();

  testAOOStart();
  testAOORequestStart();
  testAOOStopSink();
  testAOOStopSource();
  testAOOData();
  testAOOResendData();
  testAOOPingSink();
  testAOOPingSource();
  testAOOPongSink();
  testAOOPongSource();
  testAOOInvite();
  testAOOUninvite();
  testAOODecline();
  testBinMsgDetection();
  testBinDataSimple();
  testBinDataWithFlags();
  testBinDataLargeIDs();
  testBinDataXRun();
  testBinDataTooShort();

  Serial.println("All tests completed");
}

void loop() {}