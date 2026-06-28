#pragma once
#include <memory>
#include "AOOConfig.h"
#include "AudioTools/AudioCodecs/AudioCodecs.h"
#include "AudioTools/AudioCodecs/AudioEncoded.h"
#include "AudioTools/CoreAudio/AudioStreamsConverter.h"
#include "aoo/AOOBufferView.h"
#include "aoo/AOOMessageHandler.h"
#include "aoo/AOOPacketRecovery.h"

namespace arduino_aoo {

/**
 * @brief Pull-based audio sink for AOO (Audio Over OSC).
 *
 * Receives encoded audio data via the indicated transport stream,
 * buffers it per source, and provides decoded/mixed PCM audio through
 * the AudioStream readBytes() interface.
 *
 * Each remote source gets its own encoded data buffer, decoder,
 * format converter, and resampler. Decoded audio from all sources is
 * mixed via InputMixer and pulled on demand through readBytes().
 *
 * @ingroup aoo
 * @author Phil Schatzmann
 * @copyright GPLv3
 */
class AOOReceiver : public AudioStream, public AOOMessageListener {
 public:
  /// Default constructor; registers the built-in PCM decoder
  AOOReceiver() {
    addDecoder("pcm",
               []() { return (AudioDecoder *)new DecoderNetworkFormat(); });
  }

  /// @param id unique sink identifier used in AOO addressing
  /// @param io transport stream for receiving/sending OSC messages
  AOOReceiver(int id, AOOStream &io) : AOOReceiver() {
    aoo_cfg.id = id;
    setStream(io);
    msg_handler.setListener(*this);
    msg_handler.setAddressPrefix("/aoo/sink/");
    msg_handler.setId(id);
  }

  ~AOOReceiver() { end(); }

  /// Defines the communication stream for receiving/sending AOO messages
  void setStream(AOOStream &io) {
    p_io = &io;
    msg_handler.setStream(io);
  }

  /// Adds a new Decoder: provide a callback that returns a new instance of a
  /// configured decoder
  void addDecoder(const char *id, AudioDecoder *(*cb)()) {
    codec_factory.addDecoder(id, cb);
  }

  /// Get the current sink ID
  int id() { return aoo_cfg.id; }

  /// Provides a default configuration pre-filled with current values
  AOOReceiverConfig defaultConfig() { return aoo_cfg; }

  /// Starts the processing with full configuration
  bool begin(AOOReceiverConfig cfg) {
    aoo_cfg = cfg;
    return begin();
  }

  bool begin() {
    if (aoo_cfg.bits_per_sample != 16 && aoo_cfg.bits_per_sample != 0) {
      LOGE("Only 16 bits_per_sample supported, got %d", aoo_cfg.bits_per_sample);
      return false;
    }
    if (p_io == nullptr) {
      LOGE("Input not set");
      return false;
    }
    if (!p_io->begin()) {
      LOGE("Stream begin failed");
      return false;
    }
    mixer.begin(aoo_cfg);
    mixer.setRetryCount(0);              // don't retry on empty sources
    mixer.setLimitToAvailableData(false); // don't stall on slow sources
    is_active = true;
    return true;
  }

  /// Stops processing and releases all decoders
  void end() {
    is_active = false;
    for (auto &p : sources) {
      if (p->p_decoder != nullptr) {
        p->p_decoder->end();
        delete p->p_decoder;
      }
    }
    sources.clear();
    mixer.end();
  }

  /// Defines the output audio info (and target for the FormatConverter)
  void setAudioInfo(AudioInfo info) override {
    if (info.bits_per_sample != 16 && info.bits_per_sample != 0) {
      LOGE("Only 16 bits_per_sample supported, got %d", info.bits_per_sample);
      return;
    }
    LOGI("AOOReceiver::setAudioInfo: rate=%d ch=%d bits=%d", info.sample_rate,
         info.channels, info.bits_per_sample);
    AudioStream::setAudioInfo(info);
    aoo_cfg.sample_rate = info.sample_rate;
    aoo_cfg.channels = info.channels;
    aoo_cfg.bits_per_sample = info.bits_per_sample;
    for (auto &p : sources) {
      p->format_converter.begin(p->source_audio_info, aoo_cfg);
    }
  }

  /// Pull decoded, mixed audio data
  size_t readBytes(uint8_t *data, size_t len) override {
    if (!is_active || sources.empty()) return 0;

    // Process incoming network messages (stores encoded data)
    processMessages();

    // Post-processing: resend requests, drift adjustment, source management
    postProcessing();

    // Pull decoded, mixed audio from InputMixer
    return mixer.readBytes(data, len);
  }

  /// Returns the number of bytes available for reading
  int available() override {
    if (!is_active) return 0;
    processMessages();
    return mixer.available();
  }

  /// Not used in pull mode
  size_t write(const uint8_t *data, size_t len) override { return 0; }

  /// Checks if sink is active
  bool isActive() { return is_active; }

  /// Provides the number of sources that will be mixed
  int getSourceCount() { return sources.size(); }

  /// Returns the total number of buffer underruns across all sources
  uint32_t xrunCount() {
    uint32_t total = 0;
    for (auto &p : sources) {
      total += p->stats_tracker.stats().total_gaps;
    }
    return total;
  }

  /// Resets the xrun counters for all sources
  void resetXrunCount() {
    for (auto &p : sources) {
      p->stats_tracker.reset();
    }
  }

  /// Receiver-specific statistics (data path)
  struct Stats {
    uint32_t no_decoder = 0;
    uint32_t assembly_started = 0;
    uint32_t assembly_completed = 0;
    uint32_t single_frame = 0;
    uint32_t resend_requests = 0;
    uint32_t segments_stored = 0;
  };
  const Stats &stats() const { return stats_; }
  void resetStats() { stats_ = {}; }

  /// Message-level statistics (from AOOMessageHandler)
  const AOOMessageHandler::Stats &messageStats() const {
    return msg_handler.stats();
  }

  /// Request a source to re-send its start message (e.g. after a restart)
  bool requestStart(int source_id) {
    if (p_io == nullptr) return false;
    AOORequestStart req;
    req.source_id = source_id;
    req.sink_id = aoo_cfg.id;
    req.version = AOO_VERSION;
    return req.send(*p_io);
  }

  /// Send an invite to a source, requesting it to stream to this sink
  bool invite(int source_id, int32_t token = 0) {
    if (p_io == nullptr) return false;
    AOOInvite inv;
    inv.source_id = source_id;
    inv.sink_id = aoo_cfg.id;
    inv.stream_id = token;
    return inv.send(*p_io);
  }

  /// Send an uninvite to a source, asking it to stop streaming
  bool uninvite(int source_id, int32_t token = 0) {
    if (p_io == nullptr) return false;
    AOOUninvite uninv;
    uninv.source_id = source_id;
    uninv.sink_id = aoo_cfg.id;
    uninv.stream_id = token;
    return uninv.send(*p_io);
  }

  /// Access clock synchronization state
  AOOClockSync &clockSync() { return msg_handler.clockSync(); }

  AudioInfo audioInfo() override { return aoo_cfg; }

 protected:
  /**
   * @brief Per-source state: encoded data buffer, decoder, format converter,
   * resampler, packet recovery, and drift estimator.
   *
   * Each source stores ENCODED data in an IndexedRingBuffer. The pull
   * pipeline reads: buffer_view -> EncodedAudioStream(decoder) ->
   * FormatConverterStream -> ResampleStream.
   */
  struct AAOSourceLine {
    AAOSourceLine() = default;

    // Identity
    int32_t source_id = 0;
    int32_t sink_id = 0;
    int32_t stream_id = 0;
    uint32_t last_data_time = 0;
    int32_t block_size = 0;
    int32_t codec_delay_samples = 0;
    IPAddress sender_ip;
    uint16_t sender_port = 0;
    AudioInfo source_audio_info{0, 0, 0};
    Str format_str;

    // Encoded data storage (serves as jitter buffer)
    IndexedRingBuffer<SingleBuffer<uint8_t>> encoded_buffer;
    IndexedRingBufferStreamView buffer_view;
    BufferStatsTracker stats_tracker;

    // Decoding pipeline (pull-based)
    AudioDecoder *p_decoder = nullptr;
    EncodedAudioStream encoded_stream;
    FormatConverterStream format_converter;

    // Recovery
    AOOPacketRecovery recovery;

    // Drift estimation
    uint32_t first_data_time = 0;
    uint64_t total_frames_received = 0;

    // Multi-frame assembly
    int32_t assembly_seq = -1;
    int32_t assembly_total_frames = 0;
    int32_t assembly_received = 0;
    int32_t assembly_total_size = 0;
    std::vector<uint8_t> assembly_buffer;

    /// Optional mutex for thread-safe buffer access (set by AOOReceiverTask)
    MutexBase* p_mutex = nullptr;

    /// Store an encoded segment into the ring buffer (locks if mutex set)
    bool storeEncodedData(int32_t seq, const uint8_t *data, size_t len) {
      size_t written;
      {
        LockGuard lock(p_mutex);
        written = encoded_buffer.write(seq, data, len);
      }
      if (written > 0) {
        stats_tracker.received(seq);
        buffer_view.notifySegmentReceived();
        last_data_time = millis();
        total_frames_received++;
        if (first_data_time == 0) first_data_time = millis();
      }
      return written > 0;
    }

    /// Set up the pull-based decoding pipeline
    bool setupPipeline(AudioInfo output_info, AudioDecoder *decoder,
                       int buffer_depth) {
      p_decoder = decoder;
      encoded_buffer.resize(buffer_depth);
      buffer_view.setBuffer(&encoded_buffer);
      buffer_view.setStartSeq(-1);

      // Wire: buffer_view -> EncodedAudioStream(decoder) -> FormatConverterStream
      // FormatConverterStream handles channel/bit/sample-rate conversion internally
      // (its internal ResampleStream is also used for drift compensation)
      encoded_stream.setDecoder(decoder);
      encoded_stream.setStream(buffer_view);
      decoder->setAudioInfo(source_audio_info);
      encoded_stream.begin(source_audio_info);

      format_converter.setStream(encoded_stream);
      format_converter.begin(source_audio_info, output_info);

      return true;
    }

    /// Update drift compensation by reconfiguring FormatConverterStream
    /// with the effective received sample rate
    void updateDrift(AudioInfo output_info) {
      if (first_data_time == 0 || total_frames_received < 10) return;
      uint32_t elapsed_ms = millis() - first_data_time;
      if (elapsed_ms < 1000) return;
      float effective_rate =
          (float)total_frames_received * block_size * 1000.0f / elapsed_ms;
      if (effective_rate > 0) {
        AudioInfo from = source_audio_info;
        from.sample_rate = (int)effective_rate;
        format_converter.begin(from, output_info);
      }
    }

    /// Quality percentage based on stats tracker
    float qualityPercent() const { return stats_tracker.qualityPercent(); }
  };

  bool is_active = false;
  Stats stats_;
  AOOStream *p_io = nullptr;
  AOOMessageHandler msg_handler;
  InputMixer<int16_t> mixer;
  CodecFactory codec_factory;
  std::vector<std::unique_ptr<AAOSourceLine>> sources;
  AOOReceiverConfig aoo_cfg;

  /// Find or create a source line
  AAOSourceLine &getSourceLine(int32_t source_id, int32_t sink_id,
                               int32_t stream_id) {
    TRACED();
    for (auto &p : sources) {
      if (p->source_id == source_id && p->sink_id == sink_id &&
          p->stream_id == stream_id) {
        return *p;
      }
    }
    auto p = std::unique_ptr<AAOSourceLine>(new AAOSourceLine());
    p->source_id = source_id;
    p->sink_id = sink_id;
    p->stream_id = stream_id;
    // initialize per-source packet recovery
    if (aoo_cfg.recovery_max_requests > 0) {
      p->recovery.begin(aoo_cfg.recovery_wait_ms,
                        aoo_cfg.recovery_max_requests);
    }
    sources.push_back(std::move(p));
    return *sources.back();
  }

  void removeSource(int idx) {
    auto &p = sources[idx];
    if (p->p_decoder != nullptr) {
      p->p_decoder->end();
      delete p->p_decoder;
    }
    sources.erase(sources.begin() + idx);
    rebuildMixer();
  }

  void addSourceToMixer(AAOSourceLine &source) {
    if (source.buffer_view.isPrimed()) {
      mixer.add(source.format_converter);
    }
  }

  void rebuildMixer() {
    mixer.end();
    mixer.begin(aoo_cfg);
    mixer.setRetryCount(0);
    mixer.setLimitToAvailableData(false);
    for (auto &p : sources) {
      if (p->buffer_view.isPrimed()) {
        mixer.add(p->format_converter);
      }
    }
  }

  /// Process any incoming messages via AOOMessageHandler
  bool processMessages() {
    TRACED();
    msg_handler.setLogOsc(aoo_cfg.log_osc);
    return msg_handler.processMessages() > 0;
  }

  // --- AOOMessageListener callbacks (ping/pong handled by AOOMessageHandler) ---

  bool onStart(OSCData& osc) override {
    return processStartMessage(aoo_cfg.id, osc);
  }
  bool onStop(OSCData& osc) override {
    return processStopMessage(aoo_cfg.id, osc);
  }
  bool onDecline(OSCData& osc) override {
    return processDeclineMessage(aoo_cfg.id, osc);
  }
  bool onData(AOOData& data) override {
    return onDataReceived(data);
  }
  bool onBinaryData(const uint8_t* data, size_t len) override {
    return true;
  }

  void parseStartMessage(OSCData &osc, AAOSourceLine &tmp) {
    AOOStart aoo_start;
    if (aoo_start.parse(osc.data(), osc.size())) {
      tmp.source_id = aoo_start.source_id;
      tmp.stream_id = aoo_start.stream_id;
      tmp.source_audio_info.channels = aoo_start.channels;
      tmp.source_audio_info.sample_rate = aoo_start.sample_rate;
      tmp.source_audio_info.bits_per_sample = 16;
      tmp.block_size = aoo_start.block_size;
      tmp.codec_delay_samples = aoo_start.codec_delay_samples;
      tmp.format_str = aoo_start.codec;
    } else {
      LOGE("Failed to parse start message");
    }
  }

  bool processStartMessage(int sink_id, OSCData &data) {
    TRACED();
    AAOSourceLine tmp;
    parseStartMessage(data, tmp);

    LOGI("Received start: ch=%d, rate=%d, blocksize=%d, codec=%s",
         tmp.source_audio_info.channels, tmp.source_audio_info.sample_rate,
         tmp.block_size, tmp.format_str.c_str());

    AAOSourceLine &info = getSourceLine(tmp.source_id, sink_id, tmp.stream_id);

    // Clean up existing decoder if this is a duplicate /start
    if (info.p_decoder != nullptr) {
      LOGI("Re-initializing source %d (duplicate /start)", tmp.source_id);
      info.p_decoder->end();
      delete info.p_decoder;
      info.p_decoder = nullptr;
    }

    info.source_audio_info = tmp.source_audio_info;
    info.source_id = tmp.source_id;
    info.stream_id = tmp.stream_id;
    info.block_size = tmp.block_size;
    info.codec_delay_samples = tmp.codec_delay_samples;
    info.format_str = tmp.format_str;
    info.sender_ip = p_io->senderIP();
    info.sender_port = p_io->senderPort();

    AudioDecoder *p_dec = codec_factory.createDecoder(tmp.format_str.c_str());
    if (p_dec == nullptr) {
      LOGE("Decoder not defined for: %s", tmp.format_str.c_str());
      return false;
    }

    // Update output format from source
    setAudioInfo(info.source_audio_info);

    // Set up the pull-based pipeline
    if (!info.setupPipeline(aoo_cfg, p_dec, aoo_cfg.buffer_depth)) {
      LOGE("Pipeline setup failed");
      delete p_dec;
      return false;
    }

    // Add to mixer
    addSourceToMixer(info);
    LOGI("Added source %d to mixer (%d sources)", info.source_id,
         (int)sources.size());

    return true;
  }

  bool processStopMessage(int sink_id, OSCData &data) {
    TRACED();
    AOOStopSink stop;
    if (!stop.parse(data.data(), data.size())) {
      LOGE("Failed to parse stop message");
      return false;
    }
    LOGI("Stop: source=%d, stream=%d", stop.source_id, stop.stream_id);
    for (int i = sources.size() - 1; i >= 0; i--) {
      auto &p = sources[i];
      if (p->source_id == stop.source_id &&
          p->stream_id == stop.stream_id) {
        removeSource(i);
      }
    }
    return true;
  }

  bool processDeclineMessage(int sink_id, OSCData &data) {
    TRACED();
    AOODecline decline;
    if (!decline.parse(data.data(), data.size())) {
      LOGE("Failed to parse decline message");
      return false;
    }
    LOGW("Source %d declined invitation", decline.source_id);
    return true;
  }


  /// Common data processing for both OSC and binary data messages.
  /// Stores encoded data into the source's ring buffer instead of
  /// decoding immediately.
  bool onDataReceived(AOOData &aoo_data) {
    int32_t source_id = aoo_data.source_id;
    int32_t stream_id = aoo_data.stream_id;
    int32_t seq = aoo_data.seq_no;
    int32_t total_frames = aoo_data.total_number_of_frames;
    int32_t frame_idx = aoo_data.frame_idx;
    int32_t total_size = aoo_data.total_size;

    AAOSourceLine &info = getSourceLine(source_id, aoo_cfg.id, stream_id);
    info.sender_ip = p_io->senderIP();
    info.sender_port = p_io->senderPort();

    if (info.p_decoder == nullptr) {
      stats_.no_decoder++;
      LOGW("No decoder: source=%d sink=%d stream=%d (have %d sources)",
           source_id, aoo_cfg.id, stream_id, (int)sources.size());
      // Try to reuse decoder from same source with different stream_id
      for (auto &s : sources) {
        if (s->source_id == source_id && s->p_decoder != nullptr) {
          LOGW("  Found decoder on stream=%d, reusing", s->stream_id);
          // Create a new pipeline for this source line using the same codec
          AudioDecoder *p_dec =
              codec_factory.createDecoder(s->format_str.c_str());
          if (p_dec != nullptr) {
            info.source_audio_info = s->source_audio_info;
            info.block_size = s->block_size;
            info.format_str = s->format_str;
            info.setupPipeline(aoo_cfg, p_dec, aoo_cfg.buffer_depth);
            addSourceToMixer(info);
          }
          break;
        }
      }
      if (info.p_decoder == nullptr) return false;
    }

    // Multi-frame reassembly
    if (total_frames > 1) {
      if (info.assembly_seq != seq) {
        stats_.assembly_started++;
        info.assembly_seq = seq;
        info.assembly_total_frames = total_frames;
        info.assembly_received = 0;
        info.assembly_total_size = total_size;
        info.assembly_buffer.resize(total_size);
        memset(info.assembly_buffer.data(), 0, total_size);
        LOGD("Assembly: seq=%d frames=%d total=%d", seq, total_frames,
             total_size);
      }
      int32_t max_per_frame = (total_size + total_frames - 1) / total_frames;
      int32_t offset = frame_idx * max_per_frame;
      int32_t copy_len = aoo_data.audio_data.len;
      if (offset + copy_len <= info.assembly_total_size) {
        memcpy(info.assembly_buffer.data() + offset, aoo_data.audio_data.data,
               copy_len);
      }
      info.assembly_received++;
      if (info.assembly_received < info.assembly_total_frames) {
        return true;
      }
      stats_.assembly_completed++;
      if (info.storeEncodedData(seq, info.assembly_buffer.data(),
                                info.assembly_total_size)) {
        stats_.segments_stored++;
      }
    } else {
      stats_.single_frame++;
      if (aoo_data.audio_data.data != nullptr &&
          aoo_data.audio_data.len > 0) {
        if (info.storeEncodedData(aoo_data.seq_no, aoo_data.audio_data.data,
                                  aoo_data.audio_data.len)) {
          stats_.segments_stored++;
        }
      }
    }

    return true;
  }

  /// Sends pending resend requests, adjusts resamplers, removes stale sources
  void postProcessing() {
    bool mixer_changed = false;
    for (auto &p : sources) {
      // Add newly primed sources to mixer
      if (p->buffer_view.isPrimed() &&
          mixer.indexOf(p->format_converter) < 0) {
        mixer.add(p->format_converter);
        mixer_changed = true;
        LOGI("Source %d added to mixer (primed)", p->source_id);
      }

      // Update statistics
      p->stats_tracker.updateStats(p->encoded_buffer,
                                   p->buffer_view.currentSeq());

      // Conditional resend requests
      if (aoo_cfg.recovery_max_requests > 0 &&
          p->stats_tracker.qualityPercent() >= aoo_cfg.min_quality_percent) {
        int32_t seqs[8];
        int n = p->recovery.getResendRequests(seqs, 8);
        for (int i = 0; i < n; i++) {
          aooSendRequestData(*p, seqs[i], 0);
        }
      }

      // Adaptive resampling via drift estimation
      if (aoo_cfg.adaptive_resampling) {
        p->updateDrift(aoo_cfg);
      }
    }

    // Remove poor-quality sources
    for (int i = sources.size() - 1; i >= 0; i--) {
      auto &p = sources[i];
      if (p->stats_tracker.qualityPercent() < aoo_cfg.min_source_quality &&
          p->stats_tracker.stats().total_expected > 20) {
        LOGW("Removing source %d: quality %.1f%%", p->source_id,
             p->stats_tracker.qualityPercent());
        removeSource(i);
      }
    }

    // Remove stale sources
    if (aoo_cfg.stream_timeout_ms > 0) {
      uint32_t now = millis();
      for (int i = sources.size() - 1; i >= 0; i--) {
        auto &p = sources[i];
        if (p->last_data_time > 0 &&
            (now - p->last_data_time) > (uint32_t)aoo_cfg.stream_timeout_ms) {
          LOGW("Stream timeout: removing source %d", p->source_id);
          removeSource(i);
        }
      }
    }
  }

  /// Target the stream to a specific source's address for replies
  void retargetTo(IPAddress ip, uint16_t port) {
    if ((uint32_t)ip != 0) {
      p_io->setRemote(ip, port);
    }
  }

  bool aooSendRequestData(AAOSourceLine &info, int32_t seq, int32_t frame) {
    stats_.resend_requests++;
    LOGD("Requesting resend seq %d from source %d", seq, info.source_id);
    retargetTo(info.sender_ip, info.sender_port);
    AOOResendData data;
    data.source_id = info.source_id;
    data.sink_id = aoo_cfg.id;
    data.stream_id = info.stream_id;
    AOOResendData::ResendItem item;
    item.seq = seq;
    item.frame = frame;
    data.resend_items.push_back(item);
    return data.send(*p_io);
  }

};

}  // namespace arduino_aoo
