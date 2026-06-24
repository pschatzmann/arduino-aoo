#pragma once
#include <memory>
#include "AOOConfig.h"
#include "AudioTools/AudioCodecs/AudioCodecs.h"
#include "AudioTools/AudioCodecs/AudioEncoded.h"
//#include "AudioTools/AudioCodecs/CodecOpus.h"
#include "AudioTools/CoreAudio/AudioStreamsConverter.h"
#include "AudioTools/Communication/OSCData.h"
#include "aoo/AOOStream.h"
#include "aoo/AOOBuffers.h"
#include "aoo/AOOClockSync.h"
#include "aoo/AOOPacketRecovery.h"
#include "aoo/AOOResampler.h"
#include "aoo/AOOProtocol.h"

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
class AOOReceiver {
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
    sink_id = id;
    setStream(io);
    setOutput(out);
  }

  /// @param id unique sink identifier used in AOO addressing
  /// @param io transport stream for receiving/sending OSC messages (e.g. AOOStreamUDP)
  /// @param out audio output destination
  AOOReceiver(int id, AOOStream &io, AudioOutput &out) : AOOReceiver() {
    sink_id = id;
    setStream(io);
    setOutput(out);
  }

  ~AOOReceiver() { end(); }

  /// Defines the communication stream for receiving/sending AOO messages
  void setStream(AOOStream &io) { p_io = &io; }

  /// Defines the audio output
  void setOutput(AudioOutput &out) {
    p_out = &out;
    notify_info = &out;
  }
  /// Defines the audio output
  void setOutput(AudioStream &out) {
    p_out = &out;
    notify_info = &out;
  }
  /// Defines the audio output
  void setOutput(Print &out) { p_out = &out; }

  /// Adds a new Decoder: provide a callback that returns a new instance of a configured decoder
  void addDecoder(const char *id, AudioDecoder *(*cb)()) {
    codec_factory.addDecoder(id, cb);
  }

  /// Get the current sink ID
  int id() { return sink_id; }

  /// Provides a default configuration pre-filled with current values
  AOOReceiverConfig defaultConfig() {
    AOOReceiverConfig cfg;
    cfg.copyFrom(output_info);
    cfg.id = sink_id;
    cfg.jitter_buffer_depth = jitter_depth_;
    cfg.adaptive_resampling = adaptive_resample_;
    cfg.recovery_wait_ms = recovery_wait_ms_;
    cfg.recovery_max_requests = recovery_max_requests_;
    cfg.stream_timeout_ms = stream_timeout_ms_;
    cfg.length_prefix = has_length_prefix;
    cfg.log_osc = is_log_osc_active;
    return cfg;
  }

  /// Starts the processing with full configuration
  bool begin(AOOReceiverConfig cfg) {
    if (cfg.id != 0) sink_id = cfg.id;
    jitter_depth_ = cfg.jitter_buffer_depth;
    adaptive_resample_ = cfg.adaptive_resampling;
    recovery_wait_ms_ = cfg.recovery_wait_ms;
    recovery_max_requests_ = cfg.recovery_max_requests;
    stream_timeout_ms_ = cfg.stream_timeout_ms;
    if (cfg.mixer_size > 0) mixer.resize(cfg.mixer_size);
    has_length_prefix = cfg.length_prefix;
    is_log_osc_active = cfg.log_osc;
    setAudioInfo(cfg);
    return begin();
  }

  /// Starts the processing with AudioInfo only (uses current settings)
  bool begin(AudioInfo info) {
    setAudioInfo(info);
    return begin();
  }

  bool begin() {
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
    output_info = info;
    if (notify_info != nullptr) notify_info->setAudioInfo(info);
    for (auto &p : sources) {
      p->format_converter.begin(p->audio_info, output_info);
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
      int32_t seqs[8];
      int n = p->recovery.getResendRequests(seqs, 8);
      for (int i = 0; i < n; i++) {
        aooSendRequestData(*p, seqs[i], 0);
      }
      // Adaptive resampling per source
      if (adaptive_resample_ && p->p_resampler != nullptr) {
        int fill = p->jitter.isReady() ? p->jitter.filledSlots() : 0;
        int target = jitter_depth_ > 0 ? jitter_depth_ / 2 : 0;
        p->p_resampler->adjust(fill, target);
      }
    }
    // Remove stale sources
    if (stream_timeout_ms_ > 0) {
      uint32_t now = millis();
      for (int i = sources.size() - 1; i >= 0; i--) {
        auto &p = sources[i];
        if (p->last_data_time > 0 &&
            (now - p->last_data_time) > (uint32_t)stream_timeout_ms_) {
          LOGW("Stream timeout: removing source %d", p->source_id);
          removeSource(i);
        }
      }
    }
    // Flush mixer once after all sources have contributed data
    if (has_data_this_cycle_ && !sources.empty()) mixer.flushMixer();
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

  /// Request a source to re-send its start message (e.g. after a restart)
  bool requestStart(int source_id) {
    if (p_io == nullptr) return false;
    AOORequestStart req;
    req.source_id = source_id;
    req.sink_id = sink_id;
    req.version = AOO_VERSION;
    return req.send(*p_io);
  }

  /// Send an invite to a source, requesting it to stream to this sink
  bool invite(int source_id, int32_t token = 0) {
    if (p_io == nullptr) return false;
    AOOInvite inv;
    inv.source_id = source_id;
    inv.sink_id = sink_id;
    inv.stream_id = token;
    return inv.send(*p_io);
  }

  /// Send an uninvite to a source, asking it to stop streaming
  bool uninvite(int source_id, int32_t token = 0) {
    if (p_io == nullptr) return false;
    AOOUninvite uninv;
    uninv.source_id = source_id;
    uninv.sink_id = sink_id;
    uninv.stream_id = token;
    return uninv.send(*p_io);
  }

  /// Access clock synchronization state
  AOOClockSync& clockSync() { return clock_sync_; }

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

  bool has_length_prefix = false;
  bool is_active = false;
  bool is_log_osc_active = false;
  int32_t sink_id = 0;
  AOOStream *p_io = nullptr;
  OutputMixer<int16_t> mixer;
  CodecFactory codec_factory;
  std::vector<uint8_t> aao_in_buffer;
  Print *p_out = nullptr;
  AudioInfoSupport *notify_info = nullptr;
  std::vector<std::unique_ptr<AAOSourceLine>> sources;
  AudioInfo output_info;
  AOOClockSync clock_sync_;
  int jitter_depth_ = 0;
  bool adaptive_resample_ = false;
  int recovery_wait_ms_ = AOO_RECOVERY_WAIT_MS;
  int recovery_max_requests_ = AOO_RECOVERY_MAX_REQUESTS;
  int stream_timeout_ms_ = 0;
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
    if (jitter_depth_ > 0) {
      p->jitter.begin(jitter_depth_);
    }
    p->recovery.begin(recovery_wait_ms_, recovery_max_requests_);
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

    size_t msg_size = getMessageSize();
    if (msg_size == 0) return false;

    if (aao_in_buffer.size() < msg_size) aao_in_buffer.resize(msg_size);
    size_t read = p_io->readBytes(aao_in_buffer.data(), msg_size);

    OSCData data;
    data.setLogActive(is_log_osc_active);
    if (!data.parse(aao_in_buffer.data(), read)) {
      LOGE("Failed to parse OSC message: %d", (int)read);
      return false;
    }

    const char *address = data.getAddress();
    int id = getSinkIdFromAddress(address);
    if (sink_id == 0) {
      sink_id = id;
      LOGI("Setting sink_id: %d", id);
    }
    if (id != sink_id) {
      LOGI("Message for id %d ignored for id %d", sink_id, id);
      return false;
    }

    if (strstr(address, "/start") != nullptr) {
      return processStartMessage(sink_id, data);
    } else if (strstr(address, "/stop") != nullptr) {
      return processStopMessage(sink_id, data);
    } else if (strstr(address, "/decline") != nullptr) {
      return processDeclineMessage(sink_id, data);
    } else if (strstr(address, "/pong") != nullptr) {
      return processPongMessage(sink_id, data);
    } else if (strstr(address, "/ping") != nullptr) {
      return processPingMessage(sink_id, data);
    } else if (strstr(address, "/data") != nullptr) {
      return processDataMessage(sink_id, data);
    } else {
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
    if (adaptive_resample_) {
      // chain: Decoder -> FormatConverter -> Resampler -> Mixer
      auto *resampler = new AOOResampler();
      if (!resampler->begin(output_info, mixer)) {
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
      if (!info.format_converter.begin(info.audio_info, output_info)) {
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
      if (!info.format_converter.begin(info.audio_info, output_info)) {
        LOGE("Converter failed");
        return false;
      }
    }

    if (info.mixer_idx == -1) {
      info.mixer_idx = getSourceCount() - 1;
    }
    mixer.setOutputCount(getSourceCount());
    mixer.begin();
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

  size_t getMessageSize() {
    TRACED();
    size_t msg_size = AAO_MAX_SINK_BUFFER;
    if (has_length_prefix) {
      if (p_io->available() < (int)sizeof(uint64_t)) return 0;
      uint64_t size64;
      if (p_io->readBytes((uint8_t *)&size64, sizeof(size64)) !=
          sizeof(size64)) {
        LOGE("Failed to read message size");
        return 0;
      }
      msg_size = (size_t)ntohll(size64);
    }
    return msg_size;
  }

  bool processDataMessage(int sink_id, OSCData &data) {
    TRACED();
    AOOData aoo_data;
    aoo_data.sink_id = sink_id;
    if (!aoo_data.parse(data.data(), data.size())) {
      LOGE("Failed to parse data message");
      return false;
    }

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
      LOGE("Decoder is null");
      return false;
    }

    // Multi-frame reassembly
    if (total_frames > 1) {
      if (info.assembly_seq != seq) {
        info.assembly_seq = seq;
        info.assembly_total_frames = total_frames;
        info.assembly_received = 0;
        info.assembly_total_size = total_size;
        info.assembly_buffer.resize(total_size);
        memset(info.assembly_buffer.data(), 0, total_size);
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
      if (adaptive_resample_ && info.p_resampler != nullptr) {
        info.p_resampler->updateDrift(aoo_data.timestamp, info.block_size);
      }
      deliverBlock(info, seq, info.assembly_buffer.data(),
                   info.assembly_total_size);
    } else {
      if (adaptive_resample_ && info.p_resampler != nullptr) {
        info.p_resampler->updateDrift(aoo_data.timestamp, info.block_size);
      }
      deliverBlock(info, seq, aoo_data.audio_data.data,
                   aoo_data.audio_data.len);
    }

    has_data_this_cycle_ = true;
    return true;
  }

  void deliverBlock(AAOSourceLine &info, int32_t seq, const uint8_t *data,
                    size_t len) {
    info.recovery.received(seq);

    // Codec delay compensation at stream start
    if (info.codec_delay_samples > 0 && info.last_frame < 0) {
      int bps = info.audio_info.bits_per_sample / 8;
      int delay_bytes = info.codec_delay_samples * info.audio_info.channels * bps;
      LOGI("Codec delay: inserting %d bytes of silence", delay_bytes);
      const int chunk = 256;
      uint8_t silence[chunk];
      memset(silence, 0, chunk);
      int remaining = delay_bytes;
      while (remaining > 0 && info.p_decoder != nullptr) {
        int n = remaining < chunk ? remaining : chunk;
        info.p_decoder->write(silence, n);
        remaining -= n;
      }
    }

    if (jitter_depth_ > 0) {
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
      LOGW("Xrun: %d dropped frames (source %d)", gap, info.source_id);
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
    if (info.p_decoder != nullptr) {
      info.p_decoder->write(data, len);
    }
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
    LOGI("Requesting resend seq %d from source %d", seq, info.source_id);
    retargetTo(info.sender_ip, info.sender_port);
    AOOResendData data;
    data.source_id = info.source_id;
    data.sink_id = sink_id;
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
    pong.sink_id = sink_id;
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
    sink_id = id;
    setStream(io);
    setOutput(out);
  }

  /// @param id unique sink identifier used in AOO addressing
  /// @param io transport stream for receiving/sending OSC messages (e.g. AOOStreamUDP)
  /// @param out audio output destination
  AOOReceiverSingle(int id, AOOStream &io, AudioOutput &out) : AOOReceiver() {
    sink_id = id;
    setStream(io);
    setOutput(out);
  }

  /// @param id unique sink identifier used in AOO addressing
  /// @param io transport stream for receiving/sending OSC messages (e.g. AOOStreamUDP)
  /// @param out raw print output destination
  AOOReceiverSingle(int id, AOOStream &io, Print &out) : AOOReceiver() {
    sink_id = id;
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
    if (notify_info != nullptr) p_dec->addNotifyAudioChange(*notify_info);
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
