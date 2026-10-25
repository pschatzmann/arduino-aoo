#pragma once
#include <memory>
#include "AOOConfig.h"
#include "AudioTools/AudioCodecs/AudioCodecs.h"
#include "AudioTools/AudioCodecs/AudioEncoded.h"
#include "AudioTools/CoreAudio/AudioStreamsConverter.h"
#include "AudioTools/CoreAudio/Pipeline.h"
#include "aoo/AOOMessageHandler.h"
#include "aoo/AOOSourceLine.h"

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
    msg_handler.setListener(*this);
    msg_handler.setAddressPrefix("/aoo/sink/");
  }

  /// @param id unique sink identifier used in AOO addressing
  /// @param io transport stream for receiving/sending OSC messages
  AOOReceiver(int id, AOOStream &io) : AOOReceiver() {
    aoo_cfg.id = id;
    setStream(io);
    msg_handler.setId(id);
  }

  /// Destructor; stops processing and releases resources
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

  /// Get the current sink ID (with id 0 it is assigned from the first message)
  int id() { return msg_handler.getId(); }

  /// Provides a default configuration pre-filled with current values
  AOOReceiverConfig defaultConfig() { return aoo_cfg; }

  /// Starts the processing with full configuration
  bool begin(AOOReceiverConfig cfg) {
    aoo_cfg = cfg;
    return begin();
  }

  /// Starts processing with current settings
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
    msg_handler.setId(aoo_cfg.id);
    mixer.begin(aoo_cfg);
    mixer.setRetryCount(0);              // don't retry on empty sources
    mixer.setLimitToAvailableData(false); // don't stall on slow sources
    is_active = true;
    return true;
  }

  /// Stops processing and releases all decoders
  void end() {
    is_active = false;
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
      p->setOutputInfo(p->source_audio_info, aoo_cfg);
    }
  }

  /// Pull decoded, mixed audio data
  size_t readBytes(uint8_t *data, size_t len) override {
    if (!is_active) return 0;

    // Process incoming network messages and do the post processing, unless
    // available() already did so for this same StreamCopy cycle.
    if (!just_updated_) update();
    just_updated_ = false;

    if (sources.empty()) return 0;
    if (sources.size() == 1) {
      return sources[0]->readBytes(data, len);
    }
    return mixer.readBytes(data, len);
  }

  /// Returns the number of bytes available for reading
  int available() override {
    if (!is_active) return 0;
    // the post processing adds primed sources to the mixer, so it must
    // happen before we can report any data
    update();
    just_updated_ = true;
    if (sources.empty()) return 0;
    if (sources.size() == 1) {
      return sources[0]->available();
    }
    return mixer.available();
  }

  /// Not used in pull mode
  size_t write(const uint8_t *data, size_t len) override { return 0; }

  /// Checks if sink is active
  bool isActive() { return is_active; }

  /// Provides the number of sources that will be mixed
  int getSourceCount() { return sources.size(); }

  /// Returns the total number of buffer underruns across all sources.
  /// Virtual so AOOReceiverTask can override it to add locking, even when
  /// called through a base AOOReceiver& / AOOReceiver* reference.
  virtual uint32_t xrunCount() {
    uint32_t total = 0;
    for (auto &p : sources) {
      total += p->stats_tracker.stats().total_gaps;
    }
    return total;
  }

  /// Resets the xrun counters for all sources. Virtual for the same reason
  /// as xrunCount() above.
  virtual void resetXrunCount() {
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
    uint32_t start_requests = 0;
    uint32_t segments_stored = 0;
  };
  /// Returns the receiver data-path statistics
  const Stats &stats() const { return stats_; }
  /// Resets the receiver data-path statistics
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
    req.sink_id = id();
    req.version = AOO_VERSION;
    return req.send(*p_io);
  }

  /// Send an invite to a source, requesting it to stream to this sink
  bool invite(int source_id, int32_t token = 0) {
    if (p_io == nullptr) return false;
    AOOInvite inv;
    inv.source_id = source_id;
    inv.sink_id = id();
    inv.stream_id = token;
    return inv.send(*p_io);
  }

  /// Send an uninvite to a source, asking it to stop streaming
  bool uninvite(int source_id, int32_t token = 0) {
    if (p_io == nullptr) return false;
    AOOUninvite uninv;
    uninv.source_id = source_id;
    uninv.sink_id = id();
    uninv.stream_id = token;
    return uninv.send(*p_io);
  }

  /// Access clock synchronization state
  AOOClockSync &clockSync() { return msg_handler.clockSync(); }

  /// Returns the current audio format configuration
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

  bool is_active = false;
  Stats stats_;
  AOOStream *p_io = nullptr;
  AOOMessageHandler msg_handler;
  InputMixer<int16_t> mixer;
  CodecFactory codec_factory;
  std::vector<std::unique_ptr<AOOSourceLine>> sources;
  AOOReceiverConfig aoo_cfg;
  bool just_updated_ = false;
  /// Minimum time between /start requests to the same source
  static constexpr uint32_t kStartRequestIntervalMs = 500;

  /// Processes the incoming messages and does the post processing
  void update() {
    processMessages();
    postProcessing();
  }

  /// Find or create a source line
  AOOSourceLine &getSourceLine(int32_t source_id, int32_t sink_id,
                               int32_t stream_id) {
    TRACED();
    for (auto &p : sources) {
      if (p->source_id == source_id && p->sink_id == sink_id &&
          p->stream_id == stream_id) {
        return *p;
      }
    }
    auto p = std::unique_ptr<AOOSourceLine>(new AOOSourceLine());
    // the mixer reads via Stream::readBytes(), which is not virtual in all
    // cores: never wait for data
    p->setTimeout(0);
    p->source_id = source_id;
    p->sink_id = sink_id;
    p->stream_id = stream_id;
    // initialize per-source packet recovery
    if (aoo_cfg.recovery_max_requests > 0) {
      p->recovery.begin(aoo_cfg.recovery_wait_ms,
                        aoo_cfg.recovery_max_requests);
    }
    sources.push_back(std::move(p));
    onSourceLineCreated(*sources.back());
    return *sources.back();
  }

  /// Called for each new source line (e.g. to set up locking)
  virtual void onSourceLineCreated(AOOSourceLine &line) {}

  void removeSource(int idx) {
    sources.erase(sources.begin() + idx);
    rebuildMixer();
  }

  void addSourceToMixer(AOOSourceLine &source) {
    if (source.isPrimed()) {
      mixer.add(source);
    }
  }

  void rebuildMixer() {
    mixer.end();
    mixer.begin(aoo_cfg);
    mixer.setRetryCount(0);
    mixer.setLimitToAvailableData(false);
    for (auto &p : sources) {
      if (p->isPrimed()) {
        mixer.add(*p);
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

  void parseStartMessage(OSCData &osc, AOOSourceLine &tmp) {
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

  bool onStart(OSCData &data) override {
    TRACED();
    AOOSourceLine tmp;
    parseStartMessage(data, tmp);

    LOGI("Received start: ch=%d, rate=%d, blocksize=%d, codec=%s",
         tmp.source_audio_info.channels, tmp.source_audio_info.sample_rate,
         tmp.block_size, tmp.format_str.c_str());

    AOOSourceLine &info = getSourceLine(tmp.source_id, id(), tmp.stream_id);

    // Clean up existing decoder if this is a duplicate /start
    if (info.hasDecoder()) {
      LOGI("Re-initializing source %d (duplicate /start)", tmp.source_id);
      info.end();
    }

    info.source_audio_info = tmp.source_audio_info;
    info.source_id = tmp.source_id;
    info.stream_id = tmp.stream_id;
    info.block_size = tmp.block_size;
    info.codec_delay_samples = tmp.codec_delay_samples;
    info.format_str = tmp.format_str;
    info.sender_ip = p_io->senderIP();
    info.sender_port = p_io->senderPort();

    std::unique_ptr<AudioDecoder> p_dec(
        codec_factory.createDecoder(tmp.format_str.c_str()));
    if (!p_dec) {
      LOGE("Decoder not defined for: %s", tmp.format_str.c_str());
      return false;
    }

    // The output format is defined by the configuration: each source is
    // converted to it. Optionally the first source defines it.
    if (aoo_cfg.output_follows_source && sources.size() == 1) {
      setAudioInfo(info.source_audio_info);
    }

    // Set up the pull-based pipeline
    if (!info.begin(aoo_cfg, std::move(p_dec), aoo_cfg.buffer_depth)) {
      LOGE("Pipeline setup failed");
      return false;
    }

    // Add to mixer
    addSourceToMixer(info);
    LOGI("Added source %d to mixer (%d sources)", info.source_id,
         (int)sources.size());

    return true;
  }

  bool onStop(OSCData &data) override {
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

  bool onDecline(OSCData &data) override {
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
  bool onData(AOOData &aoo_data) override {
    int32_t source_id = aoo_data.source_id;
    int32_t stream_id = aoo_data.stream_id;
    int32_t seq = aoo_data.seq_no;
    int32_t total_frames = aoo_data.total_number_of_frames;
    int32_t frame_idx = aoo_data.frame_idx;
    int32_t total_size = aoo_data.total_size;

    AOOSourceLine &info = getSourceLine(source_id, id(), stream_id);
    info.sender_ip = p_io->senderIP();
    info.sender_port = p_io->senderPort();

    if (!info.hasDecoder()) {
      stats_.no_decoder++;
      LOGD("No decoder: source=%d sink=%d stream=%d (have %d sources)",
           source_id, id(), stream_id, (int)sources.size());
      // We missed the /start message (e.g. started after the source or the
      // stream timed out): ask the source to send it again. The format of
      // another stream can not be used: it might come from a different
      // sender with the same id or a restarted sender with another codec.
      // Updating last_data_time lets the line time out if the source never
      // answers.
      info.last_data_time = millis();
      requestStartFor(info);
      return false;
    }

    // Multi-frame reassembly
    if (total_frames > 1) {
      // total_size/frame_idx come straight off the wire: reject anything
      // implausible before it can size an allocation or drive a memcpy
      // offset (a negative frame_idx would otherwise underflow `offset`
      // below and write before the start of assembly_buffer).
      constexpr int32_t kMaxAssemblySize = 1 << 20;  // 1MB safety cap
      if (total_size <= 0 || total_size > kMaxAssemblySize || frame_idx < 0 ||
          frame_idx >= total_frames) {
        LOGW(
            "Rejecting malformed multi-frame data: total_size=%d "
            "frame_idx=%d frames=%d",
            (int)total_size, (int)frame_idx, (int)total_frames);
        return false;
      }
      int32_t frame_len = aoo_data.audio_data.len;
      if (info.assembly_seq != seq) {
        stats_.assembly_started++;
        info.assembly_seq = seq;
        info.assembly_total_frames = total_frames;
        info.assembly_received = 0;
        info.assembly_total_size = total_size;
        info.assembly_frame_size = 0;
        info.assembly_done = false;
        info.assembly_buffer.resize(total_size);
        info.assembly_frames.assign(total_frames, false);
        memset(info.assembly_buffer.data(), 0, total_size);
        LOGD("Assembly: seq=%d frames=%d total=%d", seq, total_frames,
             total_size);
      }
      // ignore duplicates (redundancy, resends) and inconsistent frames
      if (info.assembly_done || total_frames != info.assembly_total_frames ||
          total_size != info.assembly_total_size ||
          info.assembly_frames[frame_idx] || frame_len <= 0) {
        return true;
      }
      // all frames except the last one have the same size: derive it from
      // a non-last frame, or from the last frame and the total size
      int32_t frame_size =
          frame_idx < total_frames - 1
              ? frame_len
              : (total_size - frame_len) / (total_frames - 1);
      if (info.assembly_frame_size == 0) info.assembly_frame_size = frame_size;
      int32_t offset = frame_idx * info.assembly_frame_size;
      if (offset < 0 || offset + frame_len > info.assembly_total_size) {
        LOGW("Rejecting frame %d of seq %d: offset %d + %d > %d",
             (int)frame_idx, (int)seq, (int)offset, (int)frame_len,
             (int)info.assembly_total_size);
        return false;
      }
      memcpy(info.assembly_buffer.data() + offset, aoo_data.audio_data.data,
             frame_len);
      info.assembly_frames[frame_idx] = true;
      info.assembly_received++;
      if (info.assembly_received < info.assembly_total_frames) {
        return true;
      }
      info.assembly_done = true;
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
      if (p->isPrimed() &&
          mixer.indexOf(*p) < 0) {
        mixer.add(*p);
        mixer_changed = true;
        LOGI("Source %d added to mixer (primed)", p->source_id);
      }

      // Update statistics
      p->updateStats();

      // Conditional resend requests (not for packets which were already
      // replaced by silence)
      p->recovery.discardUpTo(p->buffer_view.currentSeq());
      if (aoo_cfg.recovery_max_requests > 0 &&
          p->stats_tracker.recentQualityPercent() >=
              aoo_cfg.min_quality_percent) {
        int32_t seqs[8];
        int n = p->recovery.getResendRequests(seqs, 8);
        for (int i = 0; i < n; i++) {
          aooSendRequestData(*p, seqs[i], 0);
        }
      }

      // Adaptive resampling via drift estimation
      if (aoo_cfg.adaptive_resampling) {
        p->updateDrift(aoo_cfg, aoo_cfg.resample_update_ms,
                       aoo_cfg.max_resample_percent);
      }
    }

    // Remove poor-quality sources
    for (int i = sources.size() - 1; i >= 0; i--) {
      auto &p = sources[i];
      if (p->stats_tracker.recentQualityPercent() <
              aoo_cfg.min_source_quality &&
          p->stats_tracker.stats().total_expected > 20) {
        LOGW("Removing source %d: quality %.1f%%", p->source_id,
             p->stats_tracker.recentQualityPercent());
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

  /// Sends a (rate limited) /start request to the source of the line
  void requestStartFor(AOOSourceLine &info) {
    uint32_t now = millis();
    if (info.last_start_request != 0 &&
        now - info.last_start_request < kStartRequestIntervalMs)
      return;
    info.last_start_request = now == 0 ? 1 : now;
    stats_.start_requests++;
    LOGI("Requesting /start from source %d", info.source_id);
    retargetTo(info.sender_ip, info.sender_port);
    requestStart(info.source_id);
  }

  bool aooSendRequestData(AOOSourceLine &info, int32_t seq, int32_t frame) {
    stats_.resend_requests++;
    LOGD("Requesting resend seq %d from source %d", seq, info.source_id);
    retargetTo(info.sender_ip, info.sender_port);
    AOOResendData data;
    data.source_id = info.source_id;
    data.sink_id = id();
    data.stream_id = info.stream_id;
    AOOResendData::ResendItem item;
    item.seq = seq;
    item.frame = frame;
    data.resend_items.push_back(item);
    return data.send(*p_io);
  }

};

}  // namespace arduino_aoo
