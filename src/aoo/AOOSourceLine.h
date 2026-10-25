#pragma once
#include <memory>
#include <vector>
#include "AudioTools/AudioCodecs/AudioEncoded.h"
#include "AudioTools/CoreAudio/AudioStreamsConverter.h"
#include "aoo/AOOBufferView.h"
#include "aoo/AOOPacketRecovery.h"

namespace arduino_aoo {

/**
 * @brief The audio of one remote source (sender) received by an AOOReceiver.
 *
 * The receiver creates one AOOSourceLine for each stream (identified by
 * source id, sink id and stream id) and stores the received encoded segments
 * with storeEncodedData(). The source line provides the decoded audio in the
 * output format of the receiver: as an AudioStream it can be read directly
 * or added to the InputMixer of the receiver.
 *
 * It consists of
 * - an IndexedRingBuffer of encoded segments (buffer_depth), which serves as
 *   jitter buffer: the audio is only provided once it is half full (priming)
 * - the decoder and a FormatConverterStream, which converts to the output
 *   format and resamples for the adaptive resampling (updateDrift())
 * - a QueueStream with the decoded audio
 * - the packet recovery (resend requests) and the statistics
 *
 * On read, the encoded segments are taken one by one from the buffer and
 * written to the decoder, so that a decoder like Opus gets exactly one packet
 * per write. A segment is only decoded when the queue has space for a
 * complete decoded segment, so that the decoder never has to wait for free
 * space. A missing segment is replaced by silence of the size of the last
 * decoded segment.
 *
 * @ingroup aoo-utils
 * @author Phil Schatzmann
 * @copyright GPLv3
 */
class AOOSourceLine : public AudioStream {
  friend class AOOReceiver;

 public:
  /// Default constructor
  AOOSourceLine() = default;

  /// Destructor; releases the decoder
  ~AOOSourceLine() { end(); }

  // --- AudioStream interface ---

  /// Provides decoded audio data (nothing before priming)
  size_t readBytes(uint8_t *data, size_t len) override {
    if (!hasDecoder() || !isPrimed()) return 0;
    decode(len);
    return decoded.readBytes(data, len);
  }

  /// Returns the number of bytes available for reading
  int available() override {
    if (!hasDecoder() || !isPrimed()) return 0;
    decode(DEFAULT_BUFFER_SIZE);
    return decoded.available();
  }

  /// Not used in pull mode
  size_t write(const uint8_t *data, size_t len) override { return 0; }

  /// Returns the AOO source identifier
  int32_t sourceId() const { return source_id; }

  // --- Statistics ---

  /// Statistics of the received segments
  const BufferStats &stats() const { return stats_tracker.stats(); }

  /// Quality percentage based on stats tracker
  float qualityPercent() const { return stats_tracker.qualityPercent(); }

  // --- Thread safety ---

  /// Set mutex for thread-safe buffer access (used by AOOReceiverTask)
  void setMutex(MutexBase *mutex) {
    p_mutex = mutex;
    buffer_view.setMutex(mutex);
    stats_tracker.setMutex(mutex);
  }

  /// Store an encoded segment into the ring buffer (locks if mutex set).
  /// Duplicates (e.g. from redundancy) and segments which are too old are
  /// ignored.
  bool storeEncodedData(int32_t seq, const uint8_t *data, size_t len) {
    size_t written = 0;
    {
      LockGuard lock(p_mutex);
      if (buffer_view.acceptsSegment(seq)) {
        written = encoded_buffer.write(seq, data, len);
      } else {
        LOGD("Segment %d ignored (read %d, newest %d)", (int)seq,
             (int)buffer_view.currentSeq(), (int)buffer_view.newestSeq());
      }
    }
    if (written > 0) {
      recovery.received(seq);
      stats_tracker.received(seq);
      buffer_view.notifySegmentReceived(seq);
      last_data_time = millis();
      data_received = true;
    }
    return written > 0;
  }

  /// Set up the decoding: decoder -> format converter -> decoded queue
  bool begin(AudioInfo output_info, std::unique_ptr<AudioDecoder> decoder,
             int buffer_depth) {
    if (p_decoder) p_decoder->end();
    p_decoder = std::move(decoder);
    encoded_buffer.resize(buffer_depth);
    buffer_view.setBuffer(&encoded_buffer);
    buffer_view.setStartSeq(-1);
    data_received = false;

    resample_rate = source_audio_info.sample_rate;
    fill_average = -1.0f;
    last_drift_update = millis();

    setupDecodedQueue(output_info);
    decoded.begin();
    last_decoded_size = 0;
    format_converter.setOutput(decoded);
    // unbuffered: the buffered resampler calls flush() on its output, which
    // clears a QueueStream; and the decoded audio of each segment must be
    // available right away
    format_converter.setBuffered(false);
    format_converter.begin(source_audio_info, output_info);
    p_decoder->setOutput(format_converter);
    p_decoder->setAudioInfo(source_audio_info);
    return p_decoder->begin();
  }

  /// Stops the decoding and releases the decoder
  void end() {
    if (p_decoder) {
      p_decoder->end();
      p_decoder.reset();
    }
    format_converter.end();
    decoded_buffer.reset();
  }

  /// Adaptive resampling: compensates the clock drift between source and
  /// sink by adjusting the playback rate so that the buffer stays half full.
  /// The rate deviates proportionally to the (smoothed) deviation of the
  /// fill level from the target, limited to max_percent. Called regularly;
  /// the rate is updated at most every update_ms.
  void updateDrift(AudioInfo output_info, uint32_t update_ms,
                   float max_percent) {
    if (!hasDecoder() || !isPrimed()) return;
    uint32_t now = millis();
    if (now - last_drift_update < update_ms) return;
    last_drift_update = now;

    float target = encoded_buffer.size() / 2.0f;
    float fill = buffer_view.bufferedSegments();
    fill_average =
        fill_average < 0 ? fill : 0.8f * fill_average + 0.2f * fill;
    float error = (fill_average - target) / target;
    if (fabsf(error) < drift_deadband) error = 0;
    float max_factor = max_percent / 100.0f;
    float correction = error * max_factor;
    if (correction > max_factor) correction = max_factor;
    if (correction < -max_factor) correction = -max_factor;

    // a higher input rate consumes the buffer faster
    int rate = (int)(source_audio_info.sample_rate * (1.0f + correction) + 0.5f);
    if (rate == resample_rate) return;
    LOGI("Adaptive resampling: fill %.1f of %d -> rate %d", fill_average,
         (int)encoded_buffer.size(), rate);
    resample_rate = rate;
    AudioInfo from = source_audio_info;
    from.sample_rate = rate;
    format_converter.begin(from, output_info);
  }

  /// Sample rate currently used for the source by the adaptive resampling
  int resampleRate() const { return resample_rate; }

  /// True once buffer is primed and ready to provide data
  bool isPrimed() const { return buffer_view.isPrimed(); }

  /// True if a decoder has been set up
  bool hasDecoder() const { return p_decoder != nullptr; }

  /// Update statistics from the buffer state
  void updateStats() {
    stats_tracker.updateStats(encoded_buffer, buffer_view.currentSeq());
  }

  /// Reconfigure format converter (e.g. when output format changes),
  /// keeping the rate of the adaptive resampling
  void setOutputInfo(AudioInfo to) {
    AudioInfo from = source_audio_info;
    if (resample_rate > 0) from.sample_rate = resample_rate;
    setupDecodedQueue(to);
    format_converter.begin(from, to);
  }

 protected:
  // Source identity
  int32_t source_id = 0;
  int32_t sink_id = 0;
  int32_t stream_id = 0;
  uint32_t last_data_time = 0;
  uint32_t last_start_request = 0;
  uint32_t start_time = 0;  // time of the /start message
  bool started = false;      // /start received
  bool data_received = false;
  bool no_data_warned = false;
  int32_t block_size = 0;
  int32_t codec_delay_samples = 0;
  IPAddress sender_ip;
  uint16_t sender_port = 0;
  AudioInfo source_audio_info{0, 0, 0};
  Str format_str;

  // Multi-frame assembly
  int32_t assembly_seq = -1;
  int32_t assembly_total_frames = 0;
  int32_t assembly_received = 0;
  int32_t assembly_total_size = 0;
  int32_t assembly_frame_size = 0;
  bool assembly_done = false;
  std::vector<uint8_t> assembly_buffer;
  std::vector<bool> assembly_frames;

  MutexBase *p_mutex = nullptr;
  BufferStatsTracker stats_tracker;
  AOOPacketRecovery recovery;
  IndexedRingBuffer<SingleBuffer<uint8_t>> encoded_buffer;
  IndexedRingBufferStreamView buffer_view;
  std::unique_ptr<AudioDecoder> p_decoder;
  FormatConverterStream format_converter;
  RingBuffer<uint8_t> decoded_buffer{0};
  QueueStream<uint8_t> decoded{decoded_buffer};
  std::vector<uint8_t> segment;
  int last_decoded_size = 0;
  /// expected maximum size of a decoded segment
  int max_decoded_size = 0;

  /// fill level deviations below this fraction of the target are ignored
  float drift_deadband = 0.25f;
  uint32_t last_drift_update = 0;
  int resample_rate = 0;
  float fill_average = -1.0f;

  /// Sizes the decoded queue for a few decoded segments in the output format
  void setupDecodedQueue(AudioInfo output_info) {
    int frames = block_size > 0 ? block_size : DEFAULT_BUFFER_SIZE;
    float rate_factor =
        source_audio_info.sample_rate > 0
            ? (float)output_info.sample_rate / source_audio_info.sample_rate
            : 1.0f;
    max_decoded_size = frames * output_info.channels *
                           (output_info.bits_per_sample / 8) *
                           (rate_factor * 1.01f) +
                       64;
    decoded_buffer.resize(std::max(4 * max_decoded_size, 8192));
  }

  /// Writes len bytes of silence to the decoded queue
  void writeSilence(int len) {
    const uint8_t zeros[64] = {0};
    while (len > 0) {
      int n = std::min(len, (int)sizeof(zeros));
      decoded.write(zeros, n);
      len -= n;
    }
  }

  /// Decodes segments until len bytes are available, nothing more can be
  /// decoded (underrun) or the queue has no space for another segment
  void decode(size_t len) {
    // decoded segments became bigger than expected: grow the (empty) queue,
    // otherwise there would never be space for a segment
    if (decoded.available() == 0 &&
        (int)decoded_buffer.size() < 4 * max_decoded_size) {
      decoded_buffer.resize(4 * max_decoded_size);
    }
    while ((size_t)decoded.available() < len &&
           decoded.availableForWrite() >= max_decoded_size) {
      auto type = buffer_view.readSegment(segment);
      if (type == IndexedRingBufferStreamView::kSegment) {
        int before = decoded.available();
        p_decoder->write(segment.data(), segment.size());
        int produced = decoded.available() - before;
        if (produced > 0) last_decoded_size = produced;
        if (produced > max_decoded_size) max_decoded_size = produced;
      } else if (type == IndexedRingBufferStreamView::kGap) {
        // missing segment: silence of the size of the last decoded segment
        writeSilence(last_decoded_size);
      } else {
        break;
      }
    }
  }

};

}  // namespace arduino_aoo
