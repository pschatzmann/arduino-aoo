#pragma once
#include "AOOConfig.h"
#include "AudioTools/AudioCodecs/AudioCodecs.h"
#include "AudioTools/AudioCodecs/AudioEncoded.h"
#include "AudioTools/CoreAudio/AudioOutput.h"
#include "aoo/AOOBuffers.h"
#include "aoo/AOOMessageHandler.h"

namespace arduino_aoo {

/**
 * @brief Audio source for AOO (Audio Over OSC) which is used to send audio data
 * via the indicated output stream (usually a AOOStreamUDP). We provide a simple
 * implementation which is purely based on the AudioTools library w/o any
 * external dependencies. The call to write() will send the data to the output
 * stream and process incoming messages (pings, resend and start requests,
 * invitations). If you pause to call write(), make sure that you continue to
 * call receive() to keep the ping alive.
 *
 * By default we transmit PCM data, but you can also use any other encoder. If
 * you decide to use a CopyEncoder to write already encoded data, you need to
 * make sure that the writes are throttled by the decoded data rate, otherwise
 * the sink will run into buffer overflows.
 *
 * Blocks which are bigger than max_frame_size are split into several frames.
 * The sent blocks are kept in a buffer (buffer_size blocks) to answer resend
 * requests. The audio can be sent to several sinks (sink_targets); sinks can
 * also add themselves with an invitation or a /start request.
 *
 * @ingroup aoo
 * @author Phil Schatzmann
 * @copyright GPLv3
 */
class AOOSender : public AudioOutput, public AOOMessageListener {
 public:
  /// @param id unique source identifier used in AOO addressing
  /// @param output transport stream for sending/receiving OSC messages (e.g.
  /// AOOStreamUDP)
  AOOSender(int id, AOOStream& output) {
    cfg.id = id;
    p_output = &output;
    msg_handler.setStream(output);
    msg_handler.setListener(*this);
    msg_handler.setAddressPrefix("/aoo/src/");
    msg_handler.setId(id);
  }
  /// Destructor; stops processing
  ~AOOSender() { end(); };

  /// Defines the output stream to which we send the AOO data
  void setStream(AOOStream& output) { p_output = &output; }

  /// Defines the encoder if we do not send PCM data
  void setEncoder(const char* format, AudioEncoder& encoder) {
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
    AOOSenderConfig result = this->cfg;
    result.copyFrom(audioInfo());
    return result;
  }

  /// Starts the processing with the indicated audio format (and the other
  /// settings from the current configuration)
  bool begin(AudioInfo info) override {
    AOOSenderConfig config = defaultConfig();
    config.copyFrom(info);
    return begin(config);
  }

  /// Starts the processing with full configuration
  bool begin(AOOSenderConfig cfg) {
    this->cfg = cfg;
    setAudioInfo(cfg);
    return begin();
  }

  /// Starts the processing with current settings
  bool begin() override {
    total_bytes_sent = 0;
    cfg.redundancy = cfg.redundancy > 0 ? cfg.redundancy : 1;
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

    if (cfg.sink_targets.empty()) {
      LOGW("No sink targets configured, defaulting to sink_id=1");
    }

    // assign random number to stream_id: random(min, max) can not cover the
    // full int32 range where long is 32 bits, so we combine two 16 bit values
    if (stream_id == 0) {
      randomSeed(micros());
      uint32_t value = ((uint32_t)random(0x10000) << 16) | random(0x10000);
      stream_id = value == 0 ? 1 : (int32_t)value;
    }

    // setup resend buffer
    aoo_out_buffer.resize(cfg.buffer_size);

    /// setup encoder
    encoder_output.source = this;
    p_encoder->setOutput(encoder_output);
    if (!p_encoder->begin(audioInfo())) {
      LOGE("Encoder failed");
      return false;
    }
    LOGI(
        "AOOSender: stream_id=%d, codec=%s, block_size=%d, cfg.redundancy=%d, "
        "cfg.codec_delay_samples=%d",
        stream_id, codecStr(), p_encoder->samplesPerFrame(), cfg.redundancy,
        cfg.codec_delay_samples);

    // The /start message announces the block size: if the encoder does not
    // define it, it is the size of the first write, so /start is sent with
    // the first block
    start_sent = false;
    if (p_encoder->samplesPerFrame() > 0) {
      if (!aoo_send_start()) {
        LOGE("Failed to send format information");
        return false;
      }
      start_sent = true;
    }
    is_streaming = true;

    return AudioOutput::begin();
  }

  /// Ends the processing: informs the sinks that the stream has stopped.
  /// Calling it again (e.g. from the destructor) has no effect.
  void end() {
    if (!is_active && !is_streaming) return;
    if (is_streaming) {
      is_streaming = false;
      forEachSink([&](int sid) {
        AOOStopSink stop;
        stop.source_id = cfg.id;
        stop.sink_id = sid;
        stop.stream_id = stream_id;
        stop.last_seq = seq_no - 1;
        return aoo_send_message(stop);
      });
    }
    if (p_encoder != nullptr) p_encoder->end();
    AudioOutput::end();
  }

  /// Encodes and sends audio data; also processes incoming ping/resend messages
  size_t write(const uint8_t* data, size_t len) override {
    // send ping message every second
    if (!receive()) {
      LOGW("Ping failed");
    }
    block_size = len;
    if (len == 0 || p_encoder == nullptr || p_output == nullptr) return 0;

    // send data
    size_t written = p_encoder->write(data, len);
    LOGD("AOOSender: wrote %d bytes of %d", (int)written, (int)len);
    return len;
  }

  /// Returns the current target sinks
  std::vector<AOOSinkTarget>& sinkTargets() { return cfg.sink_targets; }

  /// Processes incoming messages and sends ping every second;
  /// also called automatically by write()
  bool receive() {
    aoo_receive();

    // no pings before the sinks know the stream
    if (cfg.sink_targets.empty() || !start_sent) return true;

    uint64_t now = millis();
    if (now < ping_timeout) return true;
    ping_timeout = now + cfg.ping_interval_ms;

    return forEachSink([&](int sid) {
      AOOPingSink ping;
      ping.source_id = cfg.id;
      ping.sink_id = sid;
      ping.send_time = aoo_ntp_now();
      return aoo_send_message(ping);
    });
  }

  /// Access clock synchronization state (updated from pong messages)
  AOOClockSync& clockSync() { return msg_handler.clockSync(); }

  /// Returns the total number of audio bytes sent so far
  size_t getTotalBytesSent() { return total_bytes_sent; }

 protected:
  const char* encoder_format = "pcm";
  int32_t stream_id = 0;
  int32_t seq_no = 0;
  int32_t block_size = 1024;
  bool start_sent = false;
  bool is_streaming = false;
  int32_t codec_ext_data = 0;
  size_t total_bytes_sent = 0;
  uint64_t ping_timeout = 0;
  AOOSenderConfig cfg;
  AOOStream* p_output = nullptr;
  EncoderNetworkFormat pcm_encoder;
  AudioEncoder* p_encoder = &pcm_encoder;
  IndexedRingBuffer<SingleBuffer<uint8_t>> aoo_out_buffer;
  AOOMessageHandler msg_handler;

  /// Write output of encoder to the defined output stream
  class EncoderOutput : public AudioOutput {
   public:
    AOOSender* source = nullptr;
    size_t write(const uint8_t* data, size_t len) override {
      return source->aoo_send_data(data, len) ? len : 0;
    }
  } encoder_output;

  /// Determines the codec type
  const char* codecStr() { return encoder_format; }

  /// Maps bits_per_sample to AooPcmBitDepth enum (0=i8,1=i16,2=i24,3=f32,4=f64).
  /// Note: AOO has no 32 bit integer format, so 32 bits are announced as f32.
  static int32_t pcmBitDepth(int bits_per_sample) {
    switch (bits_per_sample) {
      case 8:
        return 0;
      case 16:
        return 1;
      case 24:
        return 2;
      case 32:
        return 3;
      default:
        return 1;
    }
  }

  /// Invoke a callback for each target sink. If cfg.sink_targets is empty,
  /// calls once with id=0 (broadcast). Retargets the UDP stream when the
  /// target has a non-zero IP address.
  template <typename Fn>
  bool forEachSink(Fn fn) {
    if (cfg.sink_targets.empty()) {
      return fn(1);
    }
    bool ok = true;
    for (auto& t : cfg.sink_targets) {
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

  /// Send an AOO message to the current remote address
  bool aoo_send_message(AOOMessage& msg) { return msg.send(*p_output); }

  /// Send an AOO message to the sink which sent the last received message
  bool aoo_reply(AOOMessage& msg) { return msg_handler.replyToSender(msg); }

  void fillStartMessage(AOOStart& aoo_start, int sink) {
    aoo_start.source_id = cfg.id;
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
      int bytes_per_frame =
          audioInfo().channels * (audioInfo().bits_per_sample / 8);
      aoo_start.block_size =
          bytes_per_frame > 0 ? block_size / bytes_per_frame : block_size;
    }
    aoo_start.codec = codecStr();
    aoo_start.start_time = aoo_ntp_now();
    aoo_start.codec_delay_samples = cfg.codec_delay_samples;
    if (strcmp(codecStr(), "pcm") == 0) {
      codec_ext_data = htonl(pcmBitDepth(audioInfo().bits_per_sample));
    } else if (strcmp(codecStr(), "opus") == 0) {
      codec_ext_data = htonl(2049u);  // OPUS_APPLICATION_AUDIO
    }
    aoo_start.codec_extension = {(uint8_t*)&codec_ext_data,
                                 sizeof(codec_ext_data)};
  }

  bool aoo_send_start() {
    return forEachSink([&](int sid) {
      AOOStart aoo_start;
      fillStartMessage(aoo_start, sid);
      return aoo_send_message(aoo_start);
    });
  }

  bool aoo_send_data(const uint8_t* audioData, size_t len) {
    if (!start_sent) {
      start_sent = true;
      aoo_send_start();
    }
    int32_t current_seq = seq_no++;
    uint64_t ts = aoo_ntp_now();

    bool ok = forEachSink([&](int sid) {
      for (int r = 0; r < cfg.redundancy; r++) {
        if (!aoo_send_block(sid, current_seq, audioData, len, ts, false))
          return false;
      }
      return true;
    });

    size_t written = aoo_out_buffer.write(current_seq, audioData, len);
    LOGD("AOOSender: wrote %d bytes of %d to out buffer", (int)written, (int)len);
    return ok;
  }

  /// Sends a block as one or more /data messages of at most max_frame_size
  /// bytes; all frames except the last one have the size max_frame_size.
  /// With reply = true the messages go to the sender of the last request.
  bool aoo_send_block(int32_t sink, int32_t seq, const uint8_t* audioData,
                      size_t len, uint64_t ts, bool reply) {
    int32_t num_frames = (len + cfg.max_frame_size - 1) / cfg.max_frame_size;
    for (int32_t i = 0; i < num_frames; i++) {
      int32_t offset = i * cfg.max_frame_size;
      int32_t frame_len =
          min((int32_t)(len - offset), (int32_t)cfg.max_frame_size);

      AOOData aoo_data;
      aoo_data.source_id = cfg.id;
      aoo_data.sink_id = sink;
      aoo_data.stream_id = stream_id;
      aoo_data.seq_no = seq;
      aoo_data.timestamp = ts;
      aoo_data.real_sample_rate = audioInfo().sample_rate;
      aoo_data.channel_onset = cfg.channel_onset;
      aoo_data.total_size = len;
      aoo_data.message_data_size = frame_len;
      aoo_data.total_number_of_frames = num_frames;
      aoo_data.frame_idx = i;
      aoo_data.audio_data.data = (uint8_t*)(audioData + offset);
      aoo_data.audio_data.len = frame_len;

      if (reply) {
        if (!aoo_reply(aoo_data)) return false;
      } else {
        total_bytes_sent += frame_len;
        if (!aoo_send_message(aoo_data)) return false;
      }
    }
    return true;
  }

  bool aoo_receive() {
    TRACED();
    msg_handler.setLogOsc(cfg.log_osc);
    msg_handler.processMessages();
    return true;
  }

  // --- AOOMessageListener callbacks (ping/pong handled by AOOMessageHandler) ---

  bool onStart(OSCData& osc) override { return processRequestStart(osc); }
  bool onInvite(OSCData& osc) override { return processInvite(osc); }
  bool onUninvite(OSCData& osc) override { return processUninvite(osc); }
  bool onStop(OSCData& osc) override { return processStopRequest(osc); }
  bool onResendRequest(OSCData& osc) override { return processResendRequest(osc); }

  /// Process a start request from a sink — re-send the start message
  bool processRequestStart(OSCData& osc) {
    TRACED();
    AOORequestStart req;
    if (!req.parse(osc.messageData().data, osc.messageData().len)) {
      LOGE("Failed to parse start request");
      return false;
    }
    LOGI("Start requested by sink %d", req.sink_id);
    if (!is_active) return false;
    addSinkTarget(req.sink_id);
    // otherwise /start is sent with the first block
    if (!start_sent) return true;
    AOOStart aoo_start;
    fillStartMessage(aoo_start, req.sink_id);
    return aoo_reply(aoo_start);
  }

  /// Process invite from a sink — accept by adding to sink_ids, or decline
  bool processInvite(OSCData& osc) {
    TRACED();
    AOOInvite invite;
    if (!invite.parse(osc.messageData().data, osc.messageData().len)) {
      LOGE("Failed to parse invite");
      return false;
    }
    LOGI("Invited by sink %d", invite.sink_id);
    if (!is_active) {
      AOODecline decline;
      decline.source_id = cfg.id;
      decline.sink_id = invite.sink_id;
      decline.stream_id = invite.stream_id;
      return aoo_reply(decline);
    }
    addSinkTarget(invite.sink_id);
    // otherwise /start is sent with the first block
    if (!start_sent) return true;
    AOOStart aoo_start;
    fillStartMessage(aoo_start, invite.sink_id);
    return aoo_reply(aoo_start);
  }

  /// Process uninvite from a sink — stop streaming to it
  bool processUninvite(OSCData& osc) {
    TRACED();
    AOOUninvite uninvite;
    if (!uninvite.parse(osc.messageData().data, osc.messageData().len)) {
      LOGE("Failed to parse uninvite");
      return false;
    }
    LOGI("Uninvited by sink %d", uninvite.sink_id);
    AOOStopSink stop;
    stop.source_id = cfg.id;
    stop.sink_id = uninvite.sink_id;
    stop.stream_id = stream_id;
    stop.last_seq = seq_no;
    aoo_reply(stop);
    removeSinkTarget(uninvite.sink_id);
    return true;
  }

  /// Process stop request from a sink
  bool processStopRequest(OSCData& osc) {
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
    for (auto& t : cfg.sink_targets) {
      if (t.id == id) return;
    }
    AOOSinkTarget target(id);
    if (p_output != nullptr) {
      target.ip = (uint32_t)p_output->senderIP();
      target.port = p_output->senderPort();
    }
    cfg.sink_targets.push_back(target);
  }

  /// Remove a sink target by ID
  void removeSinkTarget(int id) {
    for (auto it = cfg.sink_targets.begin(); it != cfg.sink_targets.end();
         ++it) {
      if (it->id == id) {
        cfg.sink_targets.erase(it);
        return;
      }
    }
  }

  /// request dropped packets: /aoo/src/<src>/data ,ii[ii]* <sink> <stream_id>
  /// [<seq> <frame>]*
  bool processResendRequest(OSCData& osc) {
    AOOResendData resend;
    if (!resend.parse(osc.messageData().data, osc.messageData().len)) {
      LOGE("Failed to parse resend message");
      return false;
    }
    TRACED();
    for (auto& item : resend.resend_items) {
      LOGI("Resend - seq: %d frame: %d", item.seq, item.frame);
      SingleBuffer<uint8_t>* buffer = aoo_out_buffer.get(item.seq);
      if (buffer != nullptr) {
        aoo_resend_data(item.seq, resend.sink_id, buffer->data(),
                        buffer->available());
      } else {
        LOGE("Resend: Buffer not found for seq %d", item.seq);
      }
    }
    return true;
  }

  /// Resend buffered data with the original sequence number to the sink
  /// which requested it
  bool aoo_resend_data(int32_t orig_seq, int32_t target_sink,
                       const uint8_t* audioData, size_t len) {
    return aoo_send_block(target_sink, orig_seq, audioData, len,
                          aoo_ntp_now(), true);
  }
};

}  // namespace arduino_aoo