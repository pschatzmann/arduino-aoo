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

  /// Pulls decoded audio data from the pipeline
  size_t readBytes(uint8_t *data, size_t len) override {
    return pipeline.readBytes(data, len);
  }

  /// Returns the number of bytes available for reading
  int available() override { return pipeline.available(); }

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

  /// Store an encoded segment into the ring buffer (locks if mutex set)
  bool storeEncodedData(int32_t seq, const uint8_t *data, size_t len) {
    size_t written;
    {
      LockGuard lock(p_mutex);
      written = encoded_buffer.write(seq, data, len);
    }
    if (written > 0) {
      stats_tracker.received(seq);
      buffer_view.notifySegmentReceived(seq);
      last_data_time = millis();
      total_frames_received++;
      if (first_data_time == 0) first_data_time = millis();
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

    encoded_stream.setDecoder(p_decoder.get());
    p_decoder->setAudioInfo(source_audio_info);
    format_converter.begin(source_audio_info, output_info);

    pipeline.setInput(buffer_view);
    pipeline.add(encoded_stream);
    pipeline.add(format_converter);
    pipeline.begin(source_audio_info);

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

  /// Update drift compensation by reconfiguring FormatConverterStream
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
  std::vector<uint8_t> assembly_buffer;

  MutexBase *p_mutex = nullptr;
  IndexedRingBuffer<SingleBuffer<uint8_t>> encoded_buffer;
  IndexedRingBufferStreamView buffer_view;
  std::unique_ptr<AudioDecoder> p_decoder;
  EncodedAudioStream encoded_stream;
  FormatConverterStream format_converter;
  Pipeline pipeline;
  uint32_t first_data_time = 0;
  uint64_t total_frames_received = 0;
};

}  // namespace arduino_aoo
