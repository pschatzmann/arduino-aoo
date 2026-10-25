#pragma once
#include "AOOBuffers.h"
#include "AudioTools.h"
#include "AudioTools/Concurrency/LockGuard.h"

namespace arduino_aoo {

/**
 * @brief Statistics for an IndexedRingBuffer tracking encoded audio segments.
 * @ingroup aoo-utils
 */
struct BufferStats {
  int32_t min_seq = -1;
  int32_t max_seq = -1;
  int32_t total_received = 0;
  int32_t total_expected = 0;
  int32_t total_gaps = 0;
  float quality_percent = 100.0f;
  int32_t fill_level = 0;
  int32_t continuous_available = 0;
};

/**
 * @brief Tracks statistics for an IndexedRingBuffer used as encoded data store.
 * Call received() for each stored segment and updateStats() to recompute.
 * @ingroup aoo-utils
 */
class BufferStatsTracker {
 public:
  /// Resets all statistics
  void reset() {
    stats_ = {};
    first_seq_ = -1;
    window_first_ = -1;
    window_max_ = -1;
    window_received_ = 0;
    has_previous_window_ = false;
    previous_window_quality_ = 100.0f;
  }

  /// Sets the mutex for thread-safe access
  void setMutex(MutexBase* mutex) { p_mutex = mutex; }

  /// Records that a segment with the given sequence number was received
  void received(int32_t seq) {
    stats_.total_received++;
    if (first_seq_ < 0) first_seq_ = seq;
    if (stats_.min_seq < 0 || seq < stats_.min_seq) stats_.min_seq = seq;
    if (seq > stats_.max_seq) stats_.max_seq = seq;
    updateWindow(seq);
  }

  /// Quality of the recent past: received / expected within the current
  /// window of kQualityWindow sequence numbers (or the previous window while
  /// the current one has fewer than kMinQualityWindow entries). Unlike the
  /// cumulative quality, a loss early in the stream does not keep this value
  /// low for a long time.
  float recentQualityPercent() const {
    int32_t expected = window_max_ - window_first_ + 1;
    if (window_first_ >= 0 && expected >= kMinQualityWindow) {
      return (float)window_received_ * 100.0f / expected;
    }
    return has_previous_window_ ? previous_window_quality_ : 100.0f;
  }

  /// Recomputes statistics from the current buffer state
  void updateStats(const IndexedRingBuffer<SingleBuffer<uint8_t>>& buffer,
                   int32_t read_seq) {
    LockGuard lock(p_mutex);
    stats_.fill_level = buffer.activeCount();
    if (stats_.max_seq >= 0 && stats_.min_seq >= 0) {
      stats_.total_expected = stats_.max_seq - stats_.min_seq + 1;
      stats_.total_gaps = stats_.total_expected - stats_.total_received;
      stats_.quality_percent =
          stats_.total_expected > 0
              ? (float)stats_.total_received * 100.0f / stats_.total_expected
              : 100.0f;
    }
    // Count continuous gapless segments from read position
    stats_.continuous_available = 0;
    if (!buffer.empty() && read_seq >= 0) {
      for (int32_t s = read_seq + 1; s <= stats_.max_seq; s++) {
        const auto* e = buffer.get(s);
        if (e == nullptr || !e->active) break;
        stats_.continuous_available++;
      }
    }
  }

  /// Returns the current buffer statistics
  const BufferStats& stats() const { return stats_; }
  /// Returns the quality percentage (received / expected)
  float qualityPercent() const { return stats_.quality_percent; }
  /// Returns the number of active segments in the buffer
  int32_t fillLevel() const { return stats_.fill_level; }
  /// Returns the number of gapless segments from the read position
  int32_t continuousAvailable() const { return stats_.continuous_available; }

 protected:
  static constexpr int32_t kQualityWindow = 100;
  static constexpr int32_t kMinQualityWindow = 20;
  BufferStats stats_;
  int32_t first_seq_ = -1;
  int32_t window_first_ = -1;
  int32_t window_max_ = -1;
  int32_t window_received_ = 0;
  bool has_previous_window_ = false;
  float previous_window_quality_ = 100.0f;

  void updateWindow(int32_t seq) {
    if (window_first_ < 0) {
      window_first_ = seq;
      window_max_ = seq - 1;
    }
    if (seq < window_first_) return;  // belongs to a previous window
    window_received_++;
    if (seq > window_max_) window_max_ = seq;
    int32_t expected = window_max_ - window_first_ + 1;
    if (expected >= kQualityWindow) {
      previous_window_quality_ = (float)window_received_ * 100.0f / expected;
      has_previous_window_ = true;
      window_first_ = window_max_ + 1;
      window_max_ = window_first_ - 1;
      window_received_ = 0;
    }
  }
  MutexBase* p_mutex = nullptr;
};

/**
 * @brief Stream API wrapper for IndexedRingBuffer that reads encoded segments
 * sequentially by sequence number. Used as input for EncodedAudioStream.
 *
 * Implements a priming mechanism: data is only provided once the buffer is
 * at least half full. This gives time for out-of-order packets and resend
 * requests before the consumer starts reading.
 *
 * @ingroup aoo-utils
 * @author Phil Schatzmann
 */
class IndexedRingBufferStreamView : public AudioStream {
 public:
  /// Default constructor
  IndexedRingBufferStreamView() = default;

  /// Sets the backing ring buffer
  void setBuffer(IndexedRingBuffer<SingleBuffer<uint8_t>>* buffer) {
    p_buffer = buffer;
  }

  /// Set an optional mutex for thread-safe access (used by AOOReceiverTask)
  void setMutex(MutexBase* mutex) { p_mutex = mutex; }

  /// Resets the read position to the given sequence number
  void setStartSeq(int32_t seq) {
    read_seq = seq;
    min_write_seq = -1;
    max_write_seq = -1;
    read_offset = 0;
    segments_received = 0;
    is_primed = false;
  }

  /// Returns the current read sequence number
  int32_t currentSeq() const { return read_seq; }

  /// Returns the highest received sequence number
  int32_t newestSeq() const { return max_write_seq; }

  /// Number of received segments which have not been read yet (incl. gaps)
  int32_t bufferedSegments() const {
    if (max_write_seq < 0) return 0;
    int32_t result = max_write_seq - startSeq();
    return result > 0 ? result : 0;
  }

  /// True if a segment with this seq should be stored: it must not have been
  /// read already, must not be older than the buffer window (it would
  /// overwrite a newer segment) and must not be buffered already (duplicate)
  bool acceptsSegment(int32_t seq) const {
    if (p_buffer == nullptr || p_buffer->empty()) return false;
    if (seq <= read_seq) return false;
    if (max_write_seq >= 0 &&
        seq <= max_write_seq - (int32_t)p_buffer->size())
      return false;
    const auto* entry = p_buffer->get(seq);
    return entry == nullptr || !entry->active;
  }

  /// Call when a new segment is stored in the buffer (locks if mutex set)
  void notifySegmentReceived(int32_t seq) {
    LockGuard lock(p_mutex);
    if (min_write_seq < 0 || seq < min_write_seq) min_write_seq = seq;
    if (seq > max_write_seq) max_write_seq = seq;
    segments_received++;
    if (!is_primed && p_buffer != nullptr) {
      int half = p_buffer->size() / 2;
      if (segments_received >= half) {
        is_primed = true;
        // Don't seek read_seq here: readBytes() lazily jumps to the oldest
        // available segment on the first gap it hits, which also covers the
        // case where read_seq is still -1 (nothing read yet).
        LOGI("Buffer primed after %d segments (half of %d), read from seq %d",
             segments_received, (int)p_buffer->size(), (int)(read_seq + 1));
      }
    }
  }

  /// True once the buffer has enough data to start reading
  bool isPrimed() const { return is_primed; }

  /// Returns the number of bytes available for reading
  int available() override {
    if (p_buffer == nullptr || !is_primed) return 0;
    LockGuard lock(p_mutex);
    int32_t next_seq = startSeq() + 1;
    auto* entry = p_buffer->get(next_seq);
    if (entry != nullptr && entry->active) {
      int remaining = entry->available() - read_offset;
      return remaining > 0 ? remaining : 0;
    }
    // Underrun: the next segment has not been received yet
    if (next_seq > max_write_seq) return 0;
    // Gap: report silence available so downstream doesn't stall
    return last_segment_size > 0 ? last_segment_size : DEFAULT_BUFFER_SIZE;
  }

  /// Reads encoded segments sequentially, filling gaps with silence
  size_t readBytes(uint8_t* data, size_t len) override {
    if (p_buffer == nullptr || data == nullptr || len == 0) return 0;
    if (!is_primed) return 0;

    LockGuard lock(p_mutex);
    read_seq = startSeq();
    size_t total_read = 0;
    while (total_read < len) {
      int32_t next_seq = read_seq + 1;
      auto* entry = p_buffer->get(next_seq);

      if (entry == nullptr || !entry->active) {
        // Underrun: wait for the segment instead of skipping it
        if (next_seq > max_write_seq) break;
        // If reader has fallen behind the buffer, jump to oldest available
        if (max_write_seq >= 0) {
          int32_t oldest = max_write_seq - (int32_t)p_buffer->size() + 1;
          if (oldest < 0) oldest = 0;
          if (next_seq < oldest) {
            LOGD("Reader catch-up: seq %d -> %d", (int)next_seq, (int)oldest);
            read_seq = oldest - 1;
            read_offset = 0;
            gaps_filled++;
            if (last_segment_size > 0) {
              size_t silence = std::min((size_t)last_segment_size, len - total_read);
              memset(data + total_read, 0, silence);
              total_read += silence;
            }
            break;
          }
        }
        // Normal gap: fill with silence and advance one segment
        if (last_segment_size > 0) {
          size_t silence_len = std::min((size_t)last_segment_size, len - total_read);
          memset(data + total_read, 0, silence_len);
          total_read += silence_len;
          gaps_filled++;
        }
        read_seq = next_seq;
        read_offset = 0;
        break;
      }

      int entry_avail = entry->available() - read_offset;
      if (entry_avail <= 0) {
        entry->active = false;
        read_seq = next_seq;
        read_offset = 0;
        continue;
      }

      last_segment_size = entry->available();
      size_t to_copy = std::min((size_t)entry_avail, len - total_read);
      memcpy(data + total_read, entry->address() + read_offset, to_copy);
      total_read += to_copy;
      read_offset += to_copy;

      if (read_offset >= (size_t)entry->available()) {
        entry->active = false;
        read_seq = next_seq;
        read_offset = 0;
      }
    }
    return total_read;
  }

  /// Result of readSegment()
  enum SegmentType { kNoSegment = 0, kSegment, kGap };

  /// Provides the next encoded segment in sequence order, keeping the
  /// segment boundaries (which a decoder like Opus needs): kSegment copies
  /// the segment to out, kGap reports a missing segment (the reader moves
  /// on), kNoSegment means there is nothing to read yet (not primed, or the
  /// next segment has not been received: underrun).
  SegmentType readSegment(std::vector<uint8_t>& out) {
    if (p_buffer == nullptr || !is_primed) return kNoSegment;
    LockGuard lock(p_mutex);
    read_seq = startSeq();
    int32_t next_seq = read_seq + 1;
    auto* entry = p_buffer->get(next_seq);
    if (entry == nullptr || !entry->active) {
      // Underrun: wait for the segment instead of skipping it
      if (next_seq > max_write_seq) return kNoSegment;
      // If reader has fallen behind the buffer, jump to oldest available
      int32_t oldest = max_write_seq - (int32_t)p_buffer->size() + 1;
      read_seq = next_seq < oldest ? oldest - 1 : next_seq;
      read_offset = 0;
      gaps_filled++;
      return kGap;
    }
    const uint8_t* data = entry->address();
    out.assign(data + read_offset, data + entry->available());
    entry->active = false;
    read_seq = next_seq;
    read_offset = 0;
    return kSegment;
  }

  /// Number of gaps filled with silence
  uint32_t gapsFilled() const { return gaps_filled; }

  /// Not used; data is stored via the ring buffer directly
  size_t write(const uint8_t* data, size_t len) override { return 0; }

 protected:
  /// Before the first read we start with the oldest received segment
  int32_t startSeq() const {
    if (read_seq < 0 && min_write_seq >= 0) return min_write_seq - 1;
    return read_seq;
  }

  IndexedRingBuffer<SingleBuffer<uint8_t>>* p_buffer = nullptr;
  MutexBase* p_mutex = nullptr;
  int32_t read_seq = -1;
  int32_t min_write_seq = -1;
  int32_t max_write_seq = -1;
  size_t read_offset = 0;
  int segments_received = 0;
  bool is_primed = false;
  size_t last_segment_size = 0;
  uint32_t gaps_filled = 0;
};

}  // namespace arduino_aoo
