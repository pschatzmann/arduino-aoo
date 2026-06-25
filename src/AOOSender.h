#pragma once
#include "AOOConfig.h"
#include "AudioTools/AudioCodecs/AudioCodecs.h"
#include "AudioTools/AudioCodecs/AudioEncoded.h"
#include "AudioTools/CoreAudio/AudioOutput.h"
#include "AudioTools/Communication/OSCData.h"
#include "aoo/AOOStream.h"
#include "aoo/AOOBuffers.h"
#include "aoo/AOOClockSync.h"
#include "aoo/AOOProtocol.h"

namespace arduino_aoo {

/**
 * @brief Audio source for AOO (Audio Over OSC) which is used to send audio data
 * via the indicated output stream (usually a AOOStreamUDP). We provide a simple
 * implementation which is purely based on the AudioTools library w/o any
 * external dependencies. The call to write() will send the data to the output
 * stream and receive ping and resend requests. If you pause to call write(),
 * make sure that you continue to call receive() to keep the ping alive.
 *
 * By default we tramsmit PCM data, but you can also use any other encoder. If
 * you decide to use a CopyEnoder to write already encoded data, you need to
 * make sure that the writes are throttled by the decoded data rate, otherwise
 * the AAOSink will run into buffer overflows.
 *
 * @attention Currently we do not support the split into multiple frames, so for
 * each seq no we send only one full frame. In order to support the reseding of
 * missing frames we need to define the timeout which defines how long we keep
 * the sent data in a buffer.
 *
 * Only one sink is supported!
 *
 * @ingroup aoo
 * @author Phil Schatzmann
 * @copyright GPLv3
 */
class AOOSender : public AudioOutput {
 public:
  /// @param id unique source identifier used in AOO addressing
  /// @param output transport stream for sending/receiving OSC messages (e.g. AOOStreamUDP)
  /// @param bufferTimeMs how long sent data is kept for resend requests (ms); 0 disables
  AOOSender(int id, AOOStream &output, uint16_t bufferTimeMs) {
    source_id = id;
    p_output = &output;
    aao_out_buffer.setTimeout(bufferTimeMs);
  }
  ~AOOSender() { end(); };

  /// Defines the output stream to which we send the AOO data
  void setStream(AOOStream &output) { p_output = &output; }

  /// Defines the encoder if we do not send PCM data
  void setEncoder(const char *format, AudioEncoder &encoder) {
    p_encoder = &encoder;
    encoder_format = format;
  }

  /// Resets the encoder to use PCM data
  void clearEncoder() { p_encoder = &pcm_encoder; }

  /// Updates the audio format and propagates it to the encoder
  void setAudioInfo(const AudioInfo info) override {
    if (p_encoder != nullptr) p_encoder->setAudioInfo(info);
    AudioOutput::setAudioInfo(info);
  }

  /// Provides a default configuration pre-filled with current values
  AOOSenderConfig defaultConfig() {
    AOOSenderConfig cfg;
    cfg.copyFrom(audioInfo());
    cfg.id = source_id;
    cfg.sink_targets = sink_targets;
    cfg.max_frame_size = max_frame_size;
    cfg.redundancy = redundancy;
    cfg.codec_delay_samples = codec_delay_samples;
    cfg.log_osc = is_log_osc_active;
    cfg.ping_interval_ms = ping_interval_ms;
    return cfg;
  }

  /// Starts the processing with full configuration
  bool begin(AOOSenderConfig cfg) {
    if (cfg.id != 0) source_id = cfg.id;
    sink_targets = cfg.sink_targets;
    max_frame_size = cfg.max_frame_size;
    redundancy = cfg.redundancy > 0 ? cfg.redundancy : 1;
    codec_delay_samples = cfg.codec_delay_samples;
    is_log_osc_active = cfg.log_osc;
    ping_interval_ms = cfg.ping_interval_ms;
    aao_out_buffer.setTimeout(cfg.buffer_time_ms);
    setAudioInfo(cfg);
    return begin();
  }

  /// Starts the processing with AudioInfo only (uses current settings)
  bool begin(AudioInfo info) override { return AudioOutput::begin(info); }

  /// Starts the processing with current settings
  bool begin() override {
    total_bytes_sent = 0;
    delay(500);
    if (p_encoder == nullptr) {
      LOGE("Encoder not set");
      return false;
    }
    if (p_output == nullptr) {
      LOGE("Output not set");
      return false;
    }

    if (!p_output->begin()) {
      LOGE("Stream begin failed");
      return false;
    }

    if (sink_targets.empty()) {
      LOGW("No sink targets configured, defaulting to sink_id=1");
    }

    // assign ramdom number to stream_id
    if (stream_id == 0) {
      randomSeed(millis());
      stream_id = random(-2147483648, 2147483647);
    }

    /// setup encoder
    encoder_output.source = this;
    p_encoder->setOutput(encoder_output);
    if (!p_encoder->begin(audioInfo())) {
      LOGE("Encoder failed");
      return false;
    }
    LOGW("AOOSender: stream_id=%d, codec=%s, block_size=%d, redundancy=%d, codec_delay_samples=%d",
         stream_id, codecStr(), p_encoder->samplesPerFrame(), redundancy, codec_delay_samples);

    if (!aoo_send_start()) {
      LOGE("Failed to send format information");
      return false;
    }

    return AudioOutput::begin();
  }

  /// Ends the processing
  void end() {
    if (p_encoder != nullptr) p_encoder->end();
    AudioOutput::end();
  }

  /// Encodes and sends audio data; also processes incoming ping/resend messages
  size_t write(const uint8_t *data, size_t len) override {
    // send ping message every second
    if (!receive()) {
      LOGW("Ping failed");
    }
    block_size = len;
    if (len == 0 || p_encoder == nullptr || p_output == nullptr) return 0;

    // send data
     assert(p_encoder->write(data, len) == len);
     return len;
  }

  /// Returns the current target sinks
  std::vector<AOOSinkTarget>& sinkTargets() { return sink_targets; }

  /// Processes incoming messages and sends ping every second;
  /// also called automatically by write()
  bool receive() {
    aoo_receive();

    if (sink_targets.empty()) return true;

    uint64_t now = millis();
    if (now < ping_timeout) return true;
    ping_timeout = now + ping_interval_ms;

    return forEachSink([&](int sid) {
      AOOPingSink ping;
      ping.source_id = source_id;
      ping.sink_id = sid;
      ping.send_time = micros();
      return aoo_send_message(ping);
    });
  }

  /// Access clock synchronization state (updated from pong messages)
  AOOClockSync& clockSync() { return clock_sync; }

  size_t getTotalBytesSent() { return total_bytes_sent; }

 protected:
  bool is_log_osc_active = false;
  int32_t source_id = 0;
  std::vector<AOOSinkTarget> sink_targets;
  uint64_t ping_timeout = 0;
  int ping_interval_ms = AOO_PING_INTERVAL_MS;
  AOOStream *p_output = nullptr;
  const char *encoder_format = "pcm";
  EncoderNetworkFormat pcm_encoder;
  AudioEncoder *p_encoder = &pcm_encoder;
  int32_t stream_id = 0;
  int32_t seq_no = 0;
  int32_t block_size = 1024;
  int32_t channel_onset = 0;
  int max_frame_size = 1400;
  int redundancy = 1;
  int32_t codec_delay_samples = 0;
  int32_t codec_ext_data = 0;
  AOOSourceBuffer aao_out_buffer;
  AOOClockSync clock_sync;
  std::vector<uint8_t> aao_in_buffer;
  size_t total_bytes_sent = 0;

  /// Write output of encoder to the defined output stream
  class EncoderOutput : public AudioOutput {
   public:
    AOOSender *source = nullptr;
    size_t write(const uint8_t *data, size_t len) override {
      return source->aoo_send_data(data, len) ? len : 0;
    }
  } encoder_output;

  /// Determines the codec type
  const char *codecStr() { return encoder_format; }

  /// Maps bits_per_sample to AooPcmBitDepth enum (0=i8,1=i16,2=i24,3=f32,4=f64)
  static int32_t pcmBitDepth(int bits_per_sample) {
    switch (bits_per_sample) {
      case 8:  return 0;
      case 16: return 1;
      case 24: return 2;
      case 32: return 3;
      default: return 1;
    }
  }

  /// Invoke a callback for each target sink. If sink_targets is empty,
  /// calls once with id=0 (broadcast). Retargets the UDP stream when the
  /// target has a non-zero IP address.
  template <typename Fn>
  bool forEachSink(Fn fn) {
    if (sink_targets.empty()) {
      return fn(1);
    }
    bool ok = true;
    for (auto &t : sink_targets) {
      if (t.ip != 0) setStreamTarget(t.ip, t.port);
      if (!fn(t.id)) ok = false;
    }
    return ok;
  }

  void setStreamTarget(uint32_t ip, uint16_t port) {
    if (p_output != nullptr) {
      p_output->setRemote(IPAddress(ip), port);
    }
  }

  /// Send an AOO message, prepending a length prefix if configured
  bool aoo_send_message(AOOMessage &msg) {
    return msg.send(*p_output);
  }

  void fillStartMessage(AOOStart &aoo_start, int sink) {
    aoo_start.source_id = source_id;
    aoo_start.sink_id = sink;
    aoo_start.version = AOO_VERSION;
    aoo_start.stream_id = stream_id;
    aoo_start.channels = audioInfo().channels;
    aoo_start.sample_rate = audioInfo().sample_rate;
    int enc_block = p_encoder != nullptr ? p_encoder->samplesPerFrame() : 0;
    if (enc_block > 0) {
      LOGI("Encoder block size: %d", enc_block);
      aoo_start.block_size = enc_block;
    } else {
      int bytes_per_frame = audioInfo().channels * (audioInfo().bits_per_sample / 8);
      aoo_start.block_size = bytes_per_frame > 0 ? block_size / bytes_per_frame : block_size;
    }
    aoo_start.codec = codecStr();
    aoo_start.codec_delay_samples = codec_delay_samples;
    if (strcmp(codecStr(), "pcm") == 0) {
      codec_ext_data = htonl(pcmBitDepth(audioInfo().bits_per_sample));
    } else if (strcmp(codecStr(), "opus") == 0) {
      codec_ext_data = htonl(2049u); // OPUS_APPLICATION_AUDIO
    }
    aoo_start.codec_extension = {(uint8_t *)&codec_ext_data, sizeof(codec_ext_data)};
  }

  bool aoo_send_start() {
    return forEachSink([&](int sid) {
      AOOStart aoo_start;
      fillStartMessage(aoo_start, sid);
      return aoo_send_message(aoo_start);
    });
  }

  bool aoo_send_data(const uint8_t *audioData, size_t len) {
    int32_t num_frames =
        (len + max_frame_size - 1) / max_frame_size;
    int32_t current_seq = seq_no++;
    uint64_t ts = micros();

    bool ok = forEachSink([&](int sid) {
      for (int r = 0; r < redundancy; r++) {
        for (int32_t i = 0; i < num_frames; i++) {
          int32_t offset = i * max_frame_size;
          int32_t frame_len = min((int32_t)(len - offset), (int32_t)max_frame_size);

          AOOData aoo_data;
          aoo_data.source_id = source_id;
          aoo_data.sink_id = sid;
          aoo_data.stream_id = stream_id;
          aoo_data.seq_no = current_seq;
          aoo_data.timestamp = ts;
          aoo_data.real_sample_rate = audioInfo().sample_rate;
          aoo_data.channel_onset = channel_onset;
          aoo_data.total_size = len;
          aoo_data.message_data_size = frame_len;
          aoo_data.total_number_of_frames = num_frames;
          aoo_data.frame_idx = i;
          aoo_data.audio_data.data = (uint8_t *)(audioData + offset);
          aoo_data.audio_data.len = frame_len;

          total_bytes_sent += frame_len;

          if (!aoo_send_message(aoo_data)) return false;
        }
      }
      return true;
    });

    aao_out_buffer.writeArray(current_seq, audioData, len);
    return ok;
  }

  bool aoo_receive() {
    TRACED();
    // size_t msg_size = getMessageSize();
    // if (msg_size == 0) return true;
    size_t avail = p_output->available();
    if (avail == 0) return true;

    if (aao_in_buffer.size() <  avail) aao_in_buffer.resize(avail);
    //LOGW("--> readBytes: %d", avail);
    size_t read = p_output->readBytes(aao_in_buffer.data(), avail);
    //LOGW("<-- readBytes: %d", read);

    // Stop if there is no data
    if (read == 0) {
      return true;
    }

    // Parse OSC message
    OSCData data;
    data.setLogActive(is_log_osc_active);
    if (!data.parse(aao_in_buffer.data(), read)) {
      LOGE("Failed to parse OSC message");
      return false;
    }

    // Process the received message
    const char *address = data.getAddress();
    if (StrView(address).contains("/pong")) {
      LOGD("pong");
      return processPong(data);
    } else if (StrView(address).contains("/ping")) {
      LOGD("ping");
      return processPing(data);
    } else if (StrView(address).contains("/start")) {
      LOGD("start");
      return processRequestStart(data);
    } else if (StrView(address).contains("/invite")) {
      LOGD("invite");
      return processInvite(data);
    } else if (StrView(address).contains("/uninvite")) {
      LOGD("uninvite");
      return processUninvite(data);
    } else if (StrView(address).contains("/stop")) {
      LOGI("stop");
      return processStopRequest(data);
    } else if (StrView(address).contains("/data")) {
      LOGI("data");
      return processResendRequest(data);
    } else {
      LOGW("Unknown address: %s", address);
    }
    return true;
  }

  /// Process pong message (reply to our ping)
  bool processPong(OSCData &osc) {
    TRACED();
    AOOPongSource pong;
    if (!pong.parse(osc.messageData().data, osc.messageData().len)) {
      LOGE("Failed to parse pong message");
      return false;
    }
    uint64_t t4 = micros();
    clock_sync.update(pong.t1, pong.t2, pong.t3, t4);
    LOGI("Pong: rtt=%lu us, offset=%ld us", (unsigned long)clock_sync.rttMicros(),
         (long)clock_sync.offsetMicros());
    return true;
  }

  /// Process ping message from sink, reply with pong
  bool processPing(OSCData &osc) {
    TRACED();
    AOOPingSource ping;
    if (!ping.parse(osc.messageData().data, osc.messageData().len)) {
      LOGE("Failed to parse ping message");
      return false;
    }

    AOOPongSink pong;
    pong.source_id = source_id;
    pong.sink_id = ping.sink_id;
    pong.t1 = ping.send_time;
    pong.t2 = micros();
    pong.t3 = micros();
    return aoo_send_message(pong);
  }

  /// Process a start request from a sink — re-send the start message
  bool processRequestStart(OSCData &osc) {
    TRACED();
    AOORequestStart req;
    if (!req.parse(osc.messageData().data, osc.messageData().len)) {
      LOGE("Failed to parse start request");
      return false;
    }
    LOGI("Start requested by sink %d", req.sink_id);
    if (!is_active) return false;
    addSinkTarget(req.sink_id);
    AOOStart aoo_start;
    fillStartMessage(aoo_start, req.sink_id);
    return aoo_send_message(aoo_start);
  }

  /// Process invite from a sink — accept by adding to sink_ids, or decline
  bool processInvite(OSCData &osc) {
    TRACED();
    AOOInvite invite;
    if (!invite.parse(osc.messageData().data, osc.messageData().len)) {
      LOGE("Failed to parse invite");
      return false;
    }
    LOGI("Invited by sink %d", invite.sink_id);
    if (!is_active) {
      AOODecline decline;
      decline.source_id = source_id;
      decline.sink_id = invite.sink_id;
      decline.stream_id = invite.stream_id;
      return aoo_send_message(decline);
    }
    addSinkTarget(invite.sink_id);
    AOOStart aoo_start;
    fillStartMessage(aoo_start, invite.sink_id);
    return aoo_send_message(aoo_start);
  }

  /// Process uninvite from a sink — stop streaming to it
  bool processUninvite(OSCData &osc) {
    TRACED();
    AOOUninvite uninvite;
    if (!uninvite.parse(osc.messageData().data, osc.messageData().len)) {
      LOGE("Failed to parse uninvite");
      return false;
    }
    LOGI("Uninvited by sink %d", uninvite.sink_id);
    AOOStopSink stop;
    stop.source_id = source_id;
    stop.sink_id = uninvite.sink_id;
    stop.stream_id = stream_id;
    stop.last_seq = seq_no;
    aoo_send_message(stop);
    removeSinkTarget(uninvite.sink_id);
    return true;
  }

  /// Process stop request from a sink
  bool processStopRequest(OSCData &osc) {
    TRACED();
    AOOStopSource stop;
    if (!stop.parse(osc.messageData().data, osc.messageData().len)) {
      LOGE("Failed to parse stop request");
      return false;
    }
    LOGI("Stop requested by sink %d", stop.sink_id);
    removeSinkTarget(stop.sink_id);
    return true;
  }

  /// Add a sink target if not already present (IP resolved from last packet)
  void addSinkTarget(int id) {
    for (auto &t : sink_targets) {
      if (t.id == id) return;
    }
    AOOSinkTarget target(id);
    if (p_output != nullptr) {
      target.ip = (uint32_t)p_output->senderIP();
      target.port = p_output->senderPort();
    }
    sink_targets.push_back(target);
  }

  /// Remove a sink target by ID
  void removeSinkTarget(int id) {
    for (auto it = sink_targets.begin(); it != sink_targets.end(); ++it) {
      if (it->id == id) {
        sink_targets.erase(it);
        return;
      }
    }
  }

  /// request dropped packets: /aoo/src/<src>/data ,ii[ii]* <sink> <stream_id>
  /// [<seq> <frame>]*
  bool processResendRequest(OSCData &osc) {
    AOOResendData resend;
    if (!resend.parse(osc.messageData().data, osc.messageData().len)) {
      LOGE("Failed to parse resend message");
      return false;
    }
    TRACED();
    for (auto &item : resend.resend_items) {
      LOGI("Resend - seq: %d frame: %d", item.seq, item.frame);
      SingleBuffer<uint8_t> *buffer = aao_out_buffer.getBuffer(item.seq);
      if (buffer != nullptr) {
        aoo_resend_data(item.seq, resend.sink_id, buffer->data(),
                        buffer->available());
      } else {
        LOGE("Resend: Buffer not found for seq %d", item.seq);
      }
    }
    return true;
  }

  /// Resend buffered data with the original sequence number to a specific sink
  bool aoo_resend_data(int32_t orig_seq, int32_t target_sink,
                       const uint8_t *audioData, size_t len) {
    AOOData aoo_data;
    aoo_data.source_id = source_id;
    aoo_data.sink_id = target_sink;
    aoo_data.stream_id = stream_id;
    aoo_data.seq_no = orig_seq;
    aoo_data.timestamp = micros();
    aoo_data.real_sample_rate = audioInfo().sample_rate;
    aoo_data.channel_onset = channel_onset;
    aoo_data.total_size = len;
    aoo_data.message_data_size = len;
    aoo_data.total_number_of_frames = 1;
    aoo_data.frame_idx = 0;
    aoo_data.audio_data.data = (uint8_t *)audioData;
    aoo_data.audio_data.len = len;
    return aoo_send_message(aoo_data);
  }
};

}  // namespace arduino_aoo