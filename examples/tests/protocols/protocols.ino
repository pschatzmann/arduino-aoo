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
  sink_stop.sample_offset = 1000;
  assert(sink_stop.send(queue));

  AOOStopSink rcv;
  rcv.parse(buffer.data(), buffer.size());
  
  assert(rcv.source_id == 5);
  assert(rcv.sink_id == 15);
  assert(rcv.stream_id == 25);
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
  invite.metadata = "hello";
  assert(invite.send(queue));

  AOOInvite rcv;
  assert(rcv.parse(buffer.data(), buffer.size()));

  assert(rcv.source_id == 30);
  assert(rcv.sink_id == 40);
  assert(strcmp(rcv.metadata, "hello") == 0);
}

void testAOOUninvite() {
  Serial.println("testAOOUninvite");
  buffer.clear();

  AOOUninvite uninvite;
  uninvite.source_id = 31;
  uninvite.sink_id = 41;
  assert(uninvite.send(queue));

  AOOUninvite rcv;
  assert(rcv.parse(buffer.data(), buffer.size()));

  assert(rcv.source_id == 31);
  assert(rcv.sink_id == 41);
}

void testAOODecline() {
  Serial.println("testAOODecline");
  buffer.clear();

  AOODecline decline;
  decline.source_id = 32;
  decline.sink_id = 42;
  assert(decline.send(queue));

  AOODecline rcv;
  assert(rcv.parse(buffer.data(), buffer.size()));

  assert(rcv.source_id == 32);
  assert(rcv.sink_id == 42);
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

  Serial.println("All tests completed");
}

void loop() {}