#pragma once
#include <memory>
#include "AOOConfig.h"
#include "AudioTools/AudioCodecs/AudioCodecs.h"
#include "AudioTools/AudioCodecs/AudioEncoded.h"
#include "AudioTools/CoreAudio/AudioStreamsConverter.h"
#include "AudioTools/Communication/OSCData.h"
#include "aoo/AOOStream.h"
#include "aoo/AOOBuffers.h"
#include "aoo/AOOClockSync.h"
#include "aoo/AOOPacketRecovery.h"
#include "aoo/AOOResampler.h"
#include "aoo/AOOProtocol.h"
#include "aoo/AOOBinMsg.h"

namespace arduino_aoo {

/**
 * @brief Audio sink for AOO (Audio Over OSC) which receives audio data
 * via the indicated input stream and writes it to the defined audio output.
 *
 * Each remote source gets its own decoder, format converter, jitter buffer,
 * packet recovery, and drift estimator. Decoded audio from all sources is
 * mixed via OutputMixer and written to the output.
 *
 * @ingroup aoo
 * @author Phil Schatzmann
 * @copyright GPLv3
 */
class AOOReceiver : public AudioInfoSupport, public AudioInfoSource {
 public:
  /// Default constructor; registers the built-in PCM decoder
  AOOReceiver() {
    addDecoder("pcm",
               []() { return (AudioDecoder *)new DecoderNetworkFormat(); });
  }

  /// @param id unique sink identifier used in AOO addressing
  /// @param io transport stream for receiving/sending OSC messages (e.g. AOOStreamUDP)
  /// @param out audio output destination (e.g. I2SStream)
  AOOReceiver(int id, AOOStream &io, AudioStream &out) : AOOReceiver() {
    aoo_cfg.id = id;
    setStream(io);
    setOutput(out);
  }

  /// @param id unique sink identifier used in AOO addressing
  /// @param io transport stream for receiving/sending OSC messages (e.g. AOOStreamUDP)
  /// @param out audio output destination
  AOOReceiver(int id, AOOStream &io, AudioOutput &out) : AOOReceiver() {
    aoo_cfg.id = id;
    setStream(io);
    setOutput(out);
  }

  ~AOOReceiver() { end(); }

  /// Defines the communication stream for receiving/sending AOO messages
  void setStream(AOOStream &io) { p_io = &io; }

  /// Defines the audio output
  void setOutput(AudioOutput &out) {
    p_out = &out;
    addNotifyAudioChange(out);
  }
  /// Defines the audio output
  void setOutput(AudioStream &out) {
    p_out = &out;
    addNotifyAudioChange(out);
  }
  /// Defines the audio output
  void setOutput(Print &out) { p_out = &out; }

  /// Adds a new Decoder: provide a callback that returns a new instance of a configured decoder
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
    setAudioInfo(cfg);
    return begin();
  }

  bool begin() {
    setAudioInfo(aoo_cfg);
    if (p_io == nullptr) {
      LOGE("Input not set");
      return false;
    }
    if (p_out == nullptr) {
      LOGE("Output not set");
      return false;
    }
    if (!p_io->begin()) {
      LOGE("Stream begin failed");
      return false;
    }
    if (aoo_cfg.mixer_size > 0) assert(mixer.resize(aoo_cfg.mixer_size));
    mixer.setOutput(*p_out);
    mixer.setAutoIndex(false);
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
  }

  /// Defines the output audio info (and target for the FormatConverter)
  void setAudioInfo(AudioInfo info) {
    LOGI("AOOReceiver::setAudioInfo: rate=%d ch=%d bits=%d",
         info.sample_rate, info.channels, info.bits_per_sample);
    notifyAudioChange(info);
    aoo_cfg.sample_rate = info.sample_rate;
    aoo_cfg.channels = info.channels;
    aoo_cfg.bits_per_sample = info.bits_per_sample;
    for (auto &p : sources) {
      p->format_converter.begin(p->audio_info, aoo_cfg);
    }
  }

  /// Read audio data, decode, mix, and output. Call from loop().
  bool copy() {
    if (!is_active) return false;
    has_data_this_cycle_ = false;
    bool rc = false;
    while (p_io != nullptr && p_io->available() > 0) {
      if (processMessages()) rc = true;
      else break;
    }
    postProcessing();
    return rc;
  }

  /// Sends pending resend requests, adjusts resamplers, removes stale sources
  void postProcessing() {
    // Per-source: send resend requests, adjust resampler
    for (auto &p : sources) {
      // Packet recovery: send resend requests for this source
      if (aoo_cfg.recovery_max_requests > 0) {
        int32_t seqs[8];
        int n = p->recovery.getResendRequests(seqs, 8);
        for (int i = 0; i < n; i++) {
          aooSendRequestData(*p, seqs[i], 0);
        }
      }
      // Adaptive resampling per source
      if (aoo_cfg.adaptive_resampling && p->p_resampler != nullptr) {
        int fill = p->jitter.isReady() ? p->jitter.filledSlots() : 0;
        int target = aoo_cfg.jitter_buffer_depth > 0 ? aoo_cfg.jitter_buffer_depth / 2 : 0;
        p->p_resampler->adjust(fill, target);
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
    // Flush mixer once after all sources have contributed data
    if (has_data_this_cycle_ && !sources.empty()) {
      stats_.flush_count++;
      mixer.flushMixer();
    }
  }

  /// Checks if sink is active and has received data recently
  bool isActive() { return is_active; }

  /// Provides the number of sources that will be mixed
  int getSourceCount() { return sources.size(); }

  /// Returns the total number of buffer underruns across all sources
  uint32_t xrunCount() {
    uint32_t total = 0;
    for (auto &p : sources) total += p->xrun_count;
    return total;
  }

  /// Resets the xrun counters for all sources
  void resetXrunCount() {
    for (auto &p : sources) p->xrun_count = 0;
  }

  /// Message statistics
  struct Stats {
    uint32_t binary_data = 0;
    uint32_t osc_data = 0;
    uint32_t osc_start = 0;
    uint32_t osc_stop = 0;
    uint32_t osc_ping = 0;
    uint32_t osc_pong = 0;
    uint32_t osc_other = 0;
    uint32_t parse_errors = 0;
    uint32_t no_decoder = 0;
    uint32_t decoded_bytes = 0;
    uint32_t skipped_blocks = 0;
    uint32_t assembly_started = 0;
    uint32_t assembly_completed = 0;
    uint32_t single_frame = 0;
    uint32_t resend_requests = 0;
    uint32_t deliver_called = 0;
    uint32_t null_data = 0;
    uint32_t bin_parse_fail = 0;
    uint32_t bin_wrong_id = 0;
    uint32_t xrun = 0;
    uint32_t data_with_audio = 0;
    uint32_t data_empty = 0;
    uint32_t flush_count = 0;
  };
  const Stats& stats() const { return stats_; }
  void resetStats() { stats_ = {}; }

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
  AOOClockSync& clockSync() { return clock_sync_; }

  AudioInfo audioInfo() override { return aoo_cfg; }  

 protected:
  /// Print adapter that forwards writes through a per-source resampler
  class ResamplerPrint : public Print {
   public:
    AOOResampler *p_resampler = nullptr;
    size_t write(const uint8_t *data, size_t len) override {
      if (p_resampler == nullptr) return 0;
      return p_resampler->write(data, len);
    }
    size_t write(uint8_t val) override { return write(&val, 1); }
  };

  /**
   * @brief Per-source state: decoder, format converter, jitter buffer,
   * packet recovery, drift estimator, and resampler.
   */
  struct AAOSourceLine {
    AAOSourceLine() = default;
    int32_t source_id = 0;
    int32_t sink_id = 0;
    int32_t stream_id = 0;
    uint32_t last_data_time = 0;
    bool is_active = false;
    int32_t last_frame = -1;
    int32_t block_size = 0;
    int32_t channel_onset = 0;
    int32_t codec_delay_samples = 0;
    int32_t skip_samples = 0;
    IPAddress sender_ip;
    uint16_t sender_port = 0;
    AudioInfo audio_info{0, 0, 0};
    AudioDecoder *p_decoder = nullptr;
    FormatConverterStream format_converter;
    Str format_str;
    int mixer_idx = -1;
    uint32_t xrun_count = 0;
    // per-source jitter buffer, packet recovery, resampler
    AOOJitterBuffer jitter;
    AOOPacketRecovery recovery;
    AOOResampler *p_resampler = nullptr;
    ResamplerPrint resampler_print;
    std::vector<uint8_t> jitter_tmp;
    // multi-frame assembly state
    int32_t assembly_seq = -1;
    int32_t assembly_total_frames = 0;
    int32_t assembly_received = 0;
    int32_t assembly_total_size = 0;
    std::vector<uint8_t> assembly_buffer;
  };

  bool is_active = false;
  Stats stats_;
  AOOStream *p_io = nullptr;
  OutputMixer<int16_t> mixer;
  CodecFactory codec_factory;
  std::vector<uint8_t> aao_in_buffer;
  Print *p_out = nullptr;
  std::vector<std::unique_ptr<AAOSourceLine>> sources;
  AOOReceiverConfig aoo_cfg;
  AOOClockSync clock_sync_;
  bool has_data_this_cycle_ = false;

  int getSinkIdFromAddress(const char *address) {
    if (StrView(address).startsWith("/aoo/sink/")) {
      return StrView(address + 10).toInt();
    }
    return -1;
  }

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
    // initialize per-source components
    if (aoo_cfg.jitter_buffer_depth > 0) {
      p->jitter.begin(aoo_cfg.jitter_buffer_depth);
    }
    if (aoo_cfg.recovery_max_requests > 0) {
      p->recovery.begin(aoo_cfg.recovery_wait_ms, aoo_cfg.recovery_max_requests);
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
    if (p->p_resampler != nullptr) {
      p->p_resampler->end();
      delete p->p_resampler;
    }
    sources.erase(sources.begin() + idx);
    // re-index mixer
    for (int i = 0; i < (int)sources.size(); i++) {
      sources[i]->mixer_idx = i;
    }
    mixer.setOutputCount(sources.size());
  }

  /// Process any incoming messages
  bool processMessages() {
    TRACED();
    if (p_io == nullptr || !p_io->available()) return false;

    size_t avail = p_io->available();
    if (avail == 0) return false;

    if (aao_in_buffer.size() < avail) aao_in_buffer.resize(avail);
    size_t read = p_io->readBytes(aao_in_buffer.data(), avail);

    // Check for binary message format (used by the official aoo library
    // for data messages). Binary messages have the high bit set in byte[0];
    // OSC messages start with '/' (0x2F).
    if (aoo_is_binary(aao_in_buffer.data(), read)) {
      stats_.binary_data++;
      return processBinaryMessage(aao_in_buffer.data(), read);
    }

    OSCData data;
    data.setLogActive(aoo_cfg.log_osc);
    if (!data.parse(aao_in_buffer.data(), read)) {
      stats_.parse_errors++;
      LOGE("Failed to parse OSC message: %d", (int)read);
      return false;
    }

    const char *address = data.getAddress();
    int id = getSinkIdFromAddress(address);
    if (aoo_cfg.id == 0) {
      aoo_cfg.id = id;
      LOGI("Setting sink_id: %d", id);
    }
    if (id != aoo_cfg.id) {
      LOGI("Message for id %d ignored for id %d", aoo_cfg.id, id);
      return false;
    }

    if (strstr(address, "/start") != nullptr) {
      stats_.osc_start++;
      return processStartMessage(aoo_cfg.id, data);
    } else if (strstr(address, "/stop") != nullptr) {
      stats_.osc_stop++;
      return processStopMessage(aoo_cfg.id, data);
    } else if (strstr(address, "/decline") != nullptr) {
      return processDeclineMessage(aoo_cfg.id, data);
    } else if (strstr(address, "/pong") != nullptr) {
      stats_.osc_pong++;
      return processPongMessage(aoo_cfg.id, data);
    } else if (strstr(address, "/ping") != nullptr) {
      stats_.osc_ping++;
      return processPingMessage(aoo_cfg.id, data);
    } else if (strstr(address, "/data") != nullptr) {
      stats_.osc_data++;
      return processDataMessage(aoo_cfg.id, data);
    } else {
      stats_.osc_other++;
      LOGW("Unsupported address: %s %s", address, data.getFormat());
    }
    return false;
  }

  void parseStartMessage(OSCData &osc, AAOSourceLine &tmp) {
    AOOStart aoo_start;
    if (aoo_start.parse(osc.data(), osc.size())) {
      tmp.source_id = aoo_start.source_id;
      tmp.stream_id = aoo_start.stream_id;
      tmp.audio_info.channels = aoo_start.channels;
      tmp.audio_info.sample_rate = aoo_start.sample_rate;
      tmp.audio_info.bits_per_sample = 16;
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
         tmp.audio_info.channels, tmp.audio_info.sample_rate, tmp.block_size,
         tmp.format_str.c_str());

    AAOSourceLine &info = getSourceLine(tmp.source_id, sink_id, tmp.stream_id);
    info.audio_info = tmp.audio_info;
    info.source_id = tmp.source_id;
    info.stream_id = tmp.stream_id;
    info.audio_info = tmp.audio_info;
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
    info.p_decoder = p_dec;

    return setupProcessingChain(info, p_dec);
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

  virtual bool setupProcessingChain(AAOSourceLine &info, AudioDecoder *p_dec) {
    // Update output format from the source's format
    setAudioInfo(info.audio_info);

    // Tell the decoder the source audio format
    p_dec->setAudioInfo(info.audio_info);

    if (aoo_cfg.adaptive_resampling) {
      // chain: Decoder -> FormatConverter -> Resampler -> Mixer
      auto *resampler = new AOOResampler();
      if (!resampler->begin(aoo_cfg, mixer)) {
        LOGE("Resampler failed");
        delete resampler;
        return false;
      }
      info.p_resampler = resampler;
      info.resampler_print.p_resampler = resampler;

      p_dec->setOutput(info.format_converter);
      if (!p_dec->begin()) {
        LOGE("Decoder failed");
        return false;
      }
      info.format_converter.setOutput(info.resampler_print);
      if (!info.format_converter.begin(info.audio_info, aoo_cfg)) {
        LOGE("Converter failed");
        return false;
      }
    } else {
      // chain: Decoder -> FormatConverter -> Mixer
      p_dec->setOutput(info.format_converter);
      if (!p_dec->begin()) {
        LOGE("Decoder failed");
        return false;
      }
      info.format_converter.setOutput(mixer);
      if (!info.format_converter.begin(info.audio_info, aoo_cfg)) {
        LOGE("Converter failed");
        return false;
      }
    }

    if (info.mixer_idx == -1) {
      info.mixer_idx = getSourceCount() - 1;
    }
    mixer.setOutputCount(getSourceCount());
    mixer.begin();
    if (aoo_cfg.mixer_size > 0) mixer.resize(aoo_cfg.mixer_size);
    LOGI("Mixer idx: %d for %d inputs", info.mixer_idx, (int)sources.size());
    return true;
  }

  bool processPingMessage(int sink_id, OSCData &data) {
    TRACED();
    const char *format = data.getFormat();
    if (strcmp(format, "it") != 0) {
      LOGE("Invalid ping message format: %s", format);
      return false;
    }
    int32_t source_id = data.readInt32();
    uint64_t t1 = data.readTime();
    uint64_t t2 = micros();
    aooSendPong(source_id, t1, t2);
    return true;
  }

  bool processPongMessage(int sink_id, OSCData &data) {
    TRACED();
    AOOPongSink pong;
    if (!pong.parse(data.data(), data.size())) {
      LOGE("Failed to parse pong message");
      return false;
    }
    uint64_t t4 = micros();
    clock_sync_.update(pong.t1, pong.t2, pong.t3, t4);
    LOGI("Pong: rtt=%lu us, offset=%ld us",
         (unsigned long)clock_sync_.rttMicros(),
         (long)clock_sync_.offsetMicros());
    return true;
  }


  /// Process a binary-format data message from the official aoo library
  bool processBinaryMessage(const uint8_t *data, size_t len) {
    TRACED();
    AOOData aoo_data;
    if (!aoo_parse_bin_data(data, len, aoo_data)) {
      stats_.bin_parse_fail++;
      return false;
    }

    // Check sink_id
    if (aoo_cfg.id == 0) {
      aoo_cfg.id = aoo_data.sink_id;
      LOGI("Setting sink_id from binary: %d", aoo_cfg.id);
    }
    if (aoo_data.sink_id != aoo_cfg.id) {
      stats_.bin_wrong_id++;
      return false;
    }

    return processDataFromAOOData(aoo_cfg.id, aoo_data);
  }

  bool processDataMessage(int sink_id, OSCData &data) {
    TRACED();
    AOOData aoo_data;
    aoo_data.sink_id = sink_id;
    if (!aoo_data.parse(data.data(), data.size())) {
      LOGE("Failed to parse data message");
      return false;
    }
    return processDataFromAOOData(sink_id, aoo_data);
  }

  /// Common data processing for both OSC and binary data messages
  bool processDataFromAOOData(int sink_id, AOOData &aoo_data) {
    int32_t source_id = aoo_data.source_id;
    int32_t stream_id = aoo_data.stream_id;
    int32_t seq = aoo_data.seq_no;
    int32_t total_frames = aoo_data.total_number_of_frames;
    int32_t frame_idx = aoo_data.frame_idx;
    int32_t total_size = aoo_data.total_size;

    AAOSourceLine &info = getSourceLine(source_id, sink_id, stream_id);
    info.sender_ip = p_io->senderIP();
    info.sender_port = p_io->senderPort();
    if (aoo_data.channel_onset != info.channel_onset) {
      info.channel_onset = aoo_data.channel_onset;
      if (info.channel_onset != 0) {
        LOGI("Source %d channel_onset=%d (not applied — requires mixer support)",
             source_id, info.channel_onset);
      }
    }
    mixer.setIndex(info.mixer_idx);

    if (info.p_decoder == nullptr) {
      stats_.no_decoder++;
      LOGW("No decoder: source=%d sink=%d stream=%d (have %d sources)",
           source_id, sink_id, stream_id, (int)sources.size());
      for (auto &s : sources) {
        if (s->source_id == source_id && s->p_decoder != nullptr) {
          LOGW("  Found decoder on stream=%d, reusing", s->stream_id);
          info.p_decoder = s->p_decoder;
          info.audio_info = s->audio_info;
          info.block_size = s->block_size;
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
        LOGD("Assembly: seq=%d frames=%d total=%d", seq, total_frames, total_size);
      }
      int32_t max_per_frame = (total_size + total_frames - 1) / total_frames;
      int32_t offset = frame_idx * max_per_frame;
      int32_t copy_len = aoo_data.audio_data.len;
      if (offset + copy_len <= info.assembly_total_size) {
        memcpy(info.assembly_buffer.data() + offset,
               aoo_data.audio_data.data, copy_len);
      }
      info.assembly_received++;
      if (info.assembly_received < info.assembly_total_frames) {
        return true;
      }
      stats_.assembly_completed++;
      if (aoo_cfg.adaptive_resampling && info.p_resampler != nullptr) {
        info.p_resampler->updateDrift(aoo_data.timestamp, info.block_size);
      }
      deliverBlock(info, seq, info.assembly_buffer.data(),
                   info.assembly_total_size);
    } else {
      stats_.single_frame++;
      if (aoo_data.audio_data.len > 0 && aoo_data.audio_data.data != nullptr) {
        stats_.data_with_audio++;
      } else {
        stats_.data_empty++;
      }
      if (aoo_cfg.adaptive_resampling && info.p_resampler != nullptr) {
        info.p_resampler->updateDrift(aoo_data.timestamp, info.block_size);
      }
      deliverBlock(info, seq, aoo_data.audio_data.data,
                   aoo_data.audio_data.len);
    }

    has_data_this_cycle_ = true;
    mixer.flushMixer();
    return true;
  }

  void deliverBlock(AAOSourceLine &info, int32_t seq, const uint8_t *data,
                    size_t len) {
    stats_.deliver_called++;
    if (aoo_cfg.recovery_max_requests > 0) info.recovery.received(seq);

    // Codec delay compensation: skip initial samples from decoder output
    if (info.codec_delay_samples > 0 && info.last_frame < 0) {
      info.skip_samples = info.codec_delay_samples;
      LOGD("Codec delay: will skip %d samples", info.skip_samples);
    }

    if (aoo_cfg.jitter_buffer_depth > 0) {
      info.jitter.write(seq, data, len);
      drainJitterBuffer(info);
      return;
    }

    // Direct path
    if (info.last_frame >= 0 && seq > info.last_frame + 1) {
      int gap = seq - info.last_frame - 1;
      for (int i = info.last_frame + 1; i < seq; i++) {
        writeReceivedData(info, i, nullptr, 0);
      }
      info.xrun_count += gap;
      LOGD("Xrun: %d dropped frames (source %d)", gap, info.source_id);
    }

    if (seq > info.last_frame) {
      writeReceivedData(info, seq, data, len);
      info.last_frame = seq;
    } else {
      LOGW("Out of order frame: %d < %d (source %d)", seq, info.last_frame,
           info.source_id);
      updateReceivedData(info, seq, data, len);
    }
  }

  void drainJitterBuffer(AAOSourceLine &info) {
    while (info.jitter.isReady()) {
      size_t n = info.jitter.read(info.jitter_tmp);
      int32_t seq = info.jitter.readSeq();
      if (n > 0) {
        writeReceivedData(info, seq, info.jitter_tmp.data(), n);
      } else {
        writeReceivedData(info, seq, nullptr, 0);
        info.xrun_count++;
      }
      info.last_frame = seq;
      if (info.jitter.filledSlots() == 0) break;
    }
  }

  virtual void writeReceivedData(AAOSourceLine &info, int seq,
                                 const uint8_t *data, size_t len) {
    info.last_frame = seq;
    info.last_data_time = millis();
    if (info.p_decoder == nullptr) return;
    if (info.skip_samples > 0) {
      stats_.skipped_blocks++;
      info.skip_samples--;
      return;
    }
    if (data == nullptr || len == 0) {
      stats_.xrun++;
      return;
    }
    stats_.decoded_bytes += len;
    info.p_decoder->write(data, len);
  }

  virtual void updateReceivedData(AAOSourceLine &info, int seq,
                                  const uint8_t *data, size_t len) {}

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

  bool aooSendPong(int source_id, uint64_t t1 = 0, uint64_t t2 = 0) {
    TRACED();
    if (p_io == nullptr) return false;
    retargetTo(p_io->senderIP(), p_io->senderPort());
    AOOPongSource pong;
    pong.source_id = source_id;
    pong.sink_id = aoo_cfg.id;
    pong.t1 = t1;
    pong.t2 = t2;
    pong.t3 = micros();
    return pong.send(*p_io);
  }
};

/**
 * @brief AOO Sink for a single source with minimal memory usage.
 * @ingroup aoo
 * @author Phil Schatzmann
 * @copyright GPLv3
 */
class AOOReceiverSingle : public AOOReceiver {
 public:
  /// @param id unique sink identifier used in AOO addressing
  /// @param io transport stream for receiving/sending OSC messages (e.g. AOOStreamUDP)
  /// @param out audio output destination (e.g. I2SStream)
  AOOReceiverSingle(int id, AOOStream &io, AudioStream &out) : AOOReceiver() {
    aoo_cfg.id = id;
    setStream(io);
    setOutput(out);
  }

  /// @param id unique sink identifier used in AOO addressing
  /// @param io transport stream for receiving/sending OSC messages (e.g. AOOStreamUDP)
  /// @param out audio output destination
  AOOReceiverSingle(int id, AOOStream &io, AudioOutput &out) : AOOReceiver() {
    aoo_cfg.id = id;
    setStream(io);
    setOutput(out);
  }

  /// @param id unique sink identifier used in AOO addressing
  /// @param io transport stream for receiving/sending OSC messages (e.g. AOOStreamUDP)
  /// @param out raw print output destination
  AOOReceiverSingle(int id, AOOStream &io, Print &out) : AOOReceiver() {
    aoo_cfg.id = id;
    setStream(io);
    setOutput(out);
  }

  /// Starts the single-source sink with its internal buffer and copier
  bool begin() {
    stream_id = 0;
    buffer.resize(buffer_count);
    copier.begin(*p_out, queue);
    return AOOReceiver::begin();
  }

  /// Sets up the processing chain: we write the data to a buffer first.
  bool setupProcessingChain(AAOSourceLine &info, AudioDecoder *p_dec) override {
    queue.begin(90);
    p_dec->setOutput(queue);
    //p_dec->addNotifyAudioChange(*this);
    if (!p_dec->begin()) {
      LOGE("Decoder failed");
      return false;
    }
    return true;
  }

  void writeReceivedData(AAOSourceLine &info, int seq, const uint8_t *data,
                         size_t len) override {
    if (len > buffer.size()) {
      LOGE("Buffer size too small %d > %d", (int)len, (int)buffer.size());
    }
    if (stream_id == 0) stream_id = info.stream_id;
    if (stream_id != info.stream_id) return;
    info.last_frame = seq;
    info.last_data_time = millis();
    buffer.setActualId(seq);
    size_t written = buffer.writeArray(data, len);
    if (written != len) LOGW("Buffer overflow");
  }

  void updateReceivedData(AAOSourceLine &info, int seq, const uint8_t *data,
                          size_t len) override {
    if (stream_id != info.stream_id) return;
    if (len > buffer.size()) {
      LOGE("Buffer too small %d > %d", (int)len, (int)buffer.size());
    }
    buffer.updateArray(seq, data, len);
  }

  /// Defines the buffer size
  void setBufferCount(int count) { buffer_count = count; }

 protected:
  int buffer_count = 10;
  AOOSinkBuffer buffer;
  QueueStream<uint8_t> queue{buffer};
  StreamCopy copier;
  int32_t stream_id = 0;

  void postProcessing() { copier.copy(); }
};

}  // namespace arduino_aoo
