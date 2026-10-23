#pragma once
#include <memory>
#include <vector>
#include "AudioTools/AudioCodecs/AudioEncoded.h"
#include "AudioTools/CoreAudio/AudioStreamsConverter.h"
#include "AudioTools/CoreAudio/Pipeline.h"
#include "aoo/AOOBufferView.h"
#include "aoo/AOOPacketRecovery.h"

namespace arduino_aoo {

/**
 * @brief Per-source audio stream for AOOReceiver.
 *
 * Each remote source gets its own encoded data buffer, decoder pipeline,
 * packet recovery, and drift estimator. Extends AudioStream so it can be
 * added directly to an InputMixer — readBytes() and available() delegate
 * to the internal Pipeline.
 *
 * Pipeline: IndexedRingBufferStreamView -> EncodedAudioStream -> FormatConverterStream
 *
 * @ingroup aoo
 * @author Phil Schatzmann
 * @copyright GPLv3
 */
class AOOReceiver;

class AOOSourceLine : public AudioStream {
  friend class AOOReceiver;

 public:
  /// Default constructor
  AOOSourceLine() = default;

  /// Destructor; releases the decoder
  ~AOOSourceLine() { end(); }

  // --- AudioStream interface (delegates to pipeline) ---

  /// Pulls decoded audio data from the pipeline (nothing before priming)
  size_t readBytes(uint8_t *data, size_t len) override {
    if (!hasDecoder() || !isPrimed()) return 0;
    return pipeline.readBytes(data, len);
  }

  /// Returns the number of bytes available for reading
  int available() override {
    if (!hasDecoder() || !isPrimed()) return 0;
    return pipeline.available();
  }

  /// Not used in pull mode
  size_t write(const uint8_t *data, size_t len) override { return 0; }

  /// Returns the AOO source identifier
  int32_t sourceId() const { return source_id; }

  // --- Statistics ---

  BufferStatsTracker stats_tracker;

  /// Quality percentage based on stats tracker
  float qualityPercent() const { return stats_tracker.qualityPercent(); }

  // --- Recovery ---

  AOOPacketRecovery recovery;

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
      }
    }
    if (written > 0) {
      recovery.received(seq);
      stats_tracker.received(seq);
      buffer_view.notifySegmentReceived(seq);
      last_data_time = millis();
    }
    return written > 0;
  }

  /// Set up the pull-based decoding pipeline
  bool begin(AudioInfo output_info, std::unique_ptr<AudioDecoder> decoder,
             int buffer_depth) {
    if (p_decoder) p_decoder->end();
    p_decoder = std::move(decoder);
    encoded_buffer.resize(buffer_depth);
    buffer_view.setBuffer(&encoded_buffer);
    buffer_view.setStartSeq(-1);

    // The pipeline is pulled in real time: never wait for missing data in
    // the blocking Stream::readBytes() (default timeout is 1 second)
    buffer_view.setTimeout(0);
    encoded_stream.setTimeout(0);
    format_converter.setTimeout(0);

    resample_rate = source_audio_info.sample_rate;
    fill_average = -1.0f;
    last_drift_update = millis();

    encoded_stream.setDecoder(p_decoder.get());
    p_decoder->setAudioInfo(source_audio_info);
    format_converter.begin(source_audio_info, output_info);

    pipeline.setInput(buffer_view);
    pipeline.add(encoded_stream);
    pipeline.add(format_converter);
    pipeline.begin(source_audio_info);

    // The buffer view is a live source that is momentarily empty on an
    // underrun: don't treat empty reads as end of stream and don't block
    for (ReformatBaseStream *stream :
         {(ReformatBaseStream *)&encoded_stream,
          (ReformatBaseStream *)&format_converter}) {
      stream->transformationReader().setEofOnZeroReads(false);
      stream->transformationReader().setZeroReadDelay(0);
    }

    return true;
  }

  /// Stops the pipeline and releases the decoder
  void end() {
    pipeline.end();
    if (p_decoder) {
      p_decoder->end();
      p_decoder.reset();
    }
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
    if (fabsf(error) < kDriftDeadband) error = 0;
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

  /// Reconfigure format converter (e.g. when output format changes)
  void setOutputInfo(AudioInfo from, AudioInfo to) {
    format_converter.begin(from, to);
  }

 protected:
  // Source identity
  int32_t source_id = 0;
  int32_t sink_id = 0;
  int32_t stream_id = 0;
  uint32_t last_data_time = 0;
  uint32_t last_start_request = 0;
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
  IndexedRingBuffer<SingleBuffer<uint8_t>> encoded_buffer;
  IndexedRingBufferStreamView buffer_view;
  std::unique_ptr<AudioDecoder> p_decoder;
  EncodedAudioStream encoded_stream;
  FormatConverterStream format_converter;
  Pipeline pipeline;
  /// fill level deviations below this fraction of the target are ignored
  static constexpr float kDriftDeadband = 0.25f;
  uint32_t last_drift_update = 0;
  int resample_rate = 0;
  float fill_average = -1.0f;
};

}  // namespace arduino_aoo
